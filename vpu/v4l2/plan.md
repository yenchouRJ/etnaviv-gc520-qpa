# VC8000NanoD on mainline Hantro (V4L2 stateless) — minimal H.264 bring-up plan

Goal: prove that the MA35D1 VC8000NanoD decodes H.264 correctly through the **unmodified
mainline G1 H.264 code** (`hantro_g1_h264_dec.c`) driven by GStreamer `v4l2slh264dec`, with a
thin MA35D1 variant. This is an experiment to decide between this route and the DRM render-node
route (`../vc8k`). Nothing here replaces `../vc8k`; the DT compatible string selects which driver binds.

## 1. Decisions (from the grilling session)

| # | Decision |
|---|---|
| D1 Goal | H.264 decodes on NanoD via mainline G1 code. Minimal: H.264 only, no MPEG-2/VP8 |
| D2 Output | NV12 (decoder native, no post-processor). ARGB/XRGB later (mainline PP has no RGB path) |
| D3 Pass | Per-frame checksum of decoded NV12 == ffmpeg software decode. Display check last |
| D4 Streams | Generated with host ffmpeg/libx264 into `vpu/videos/`, one H.264 feature per step |
| D5 Switching | Same DT node; `compatible = "nuvoton,ma35d1-vpu"` -> Hantro, `"nuvoton,ma35d1-vc8k"` -> DRM |
| D6 Memory | Keep `memory-region = <&vc8k_buf>` (32 MiB). Enlarging it is acceptable if needed |
| D7 Clock | Hantro convention: `clock-names = "vpu"`, variant `clk_names = { "vpu" }` |
| D8 Layout | Full mirror of `verisilicon/` in `vpu/v4l2/kernel/`; `sync.sh` copies it into `linux-custom` |
| D9 Variant | New `ma35d1_vpu_hw.c`, modelled on `sama5d4_vdec_hw.c` |
| D10 Quirks | Vendor quirks added up front, each behind a module parameter so it can be switched off |

## 2. Stack (bottom to top)

```
L7  gst-launch-1.0 / app                          filesrc ! h264parse ! v4l2slh264dec ! sink
L6  gst1-plugins-bad v4l2codecs (1.22.9)          parses SPS/PPS/slices, builds DPB + ref lists
--- /dev/videoN + /dev/mediaN (V4L2 stateless H.264 controls + media request API) ---
L5  V4L2 core: vb2-dma-contig, v4l2-mem2mem, request API, v4l2-h264 helpers        (mainline)
L4  hantro_drv.c / hantro_v4l2.c                                                    (mainline + match entry)
L3  hantro_g1_h264_dec.c, hantro_h264.c, hantro_g1.c (irq/reset)                   (mainline)
L2  ma35d1_vpu_hw.c: fmts, clk, irq, init, quirks                                   (NEW)
L1  VC8000NanoD @ 0x40290000, SPI 28, VC8K_GATE clock, vc8k_buf pool
```

## 3. Facts checked in the tree (6.6.93)

- `VIDEO_HANTRO` depends on `ARCH_MXC || ARCH_ROCKCHIP || ARCH_AT91 || ARCH_SUNXI || COMPILE_TEST`;
  needs `ARCH_MA35D1` added. Current kernel `.config`: `VIDEO_HANTRO` off, `V4L_MEM2MEM_DRIVERS` off,
  `MEDIA_CONTROLLER=y`, `CONFIG_ARCH_MA35D1=y`.
- Probe (`hantro_drv.c`): clocks via `devm_clk_bulk_get` (names required), reg index 0 if no
  `reg_names`, IRQ index 0 if irq name is NULL, resets optional, sets 32-bit coherent DMA mask.
- G1 H.264 start writes `G1_REG_CONFIG` (swreg2) without `TILED_MODE_LSB` (bit 7), i.e. the vendor
  "clear reg2 bit7" quirk is already satisfied for H.264. Keep a guard anyway (D10).
- `hantro_g1_reset()` writes `G1_REG_SOFT_RESET` = 0x194 (swreg101). The NanoD register map lists
  swreg100 then swreg102 — swreg101 is not documented. Risk R2.
- PP swreg91 (vendor quirk "write 0") is only relevant when the post-processor runs. With NV12
  output (D2) the PP is not used, so this quirk is dormant in the minimal test.
- NanoD has extra registers (swreg102–118, per-reference chroma bases) that mainline never writes.
  Vendor SW does not use them either (`DEC_X170_REGISTERS 60`, `PP_X170_REGISTERS 41`). Risk R3.
- Mainline PP outputs only YUYV (`VPU_PP_OUT_RGB` defined but unused). RGB is a later phase.

## 4. Phases

### V0 — Configs and tree sync
- Write `sync.sh` (rsync `kernel/` -> `$K/drivers/media/platform/verisilicon/`, with `--dry-run`
  option and a diff summary). Never deletes files outside that directory.
- User enables configs (section 7), rebuilds kernel and rootfs.
- Exit: kernel builds with `VIDEO_HANTRO=y`, `v4l2-ctl`/`media-ctl`/`gst-inspect-1.0 v4l2codecs`
  present on target.

### V1 — Variant + probe
- `ma35d1_vpu_hw.c`:
  - `ma35d1_vpu_fmts[]`: `V4L2_PIX_FMT_NV12` (raw) + `V4L2_PIX_FMT_H264_SLICE` (coded), frmsize
    48..1920 x 48..1088 step 16 (from the HW features doc, capped to 1080p).
  - `codec_ops[HANTRO_MODE_H264_DEC] = { .run = hantro_g1_h264_dec_run, .reset = ma35d1_vpu_reset,
    .init = hantro_h264_dec_init, .exit = hantro_h264_dec_exit }`.
  - irqs `{ NULL, hantro_g1_irq }` (unnamed, index 0), `clk_names = { "vpu" }`.
  - `.init`: read swreg0 ID and log it; optional quirk setup.
- `hantro_drv.c`: add `{ .compatible = "nuvoton,ma35d1-vpu", .data = &ma35d1_vpu_variant }`
  under `#ifdef CONFIG_VIDEO_HANTRO_MA35D1`.
- `hantro_hw.h`: `extern const struct hantro_variant ma35d1_vpu_variant;`
- `Kconfig`: add `ARCH_MA35D1` to `VIDEO_HANTRO` depends, new `VIDEO_HANTRO_MA35D1` (bool, default y).
- `Makefile`: `hantro-vpu-$(CONFIG_VIDEO_HANTRO_MA35D1) += ma35d1_vpu_hw.o`.
- DT: change compatible, add `clock-names = "vpu"` (section 6).
- Exit: `dmesg` shows hantro probe on 40290000, `/dev/video0` + `/dev/media0` exist,
  `v4l2-ctl --list-formats-out` shows `S264`, `--list-formats` shows `NV12`.

### V2 — Quirks (D10), behind module params of `hantro_vpu`
- `ma35d1_reg2_bit7_check` (default 1): the G1 run function writes swreg2 and starts the decoder
  in one go, so bit 7 cannot be cleared afterwards from a wrapper. Instead, read swreg2 back in
  the IRQ path and warn once if bit 7 was set. The H.264 path does not set it today; if it ever
  does, patch the write in `hantro_g1_h264_dec.c`.
- `ma35d1_soft_reset` (default 0): `ma35d1_vpu_reset` = IRQ_DIS + clock gate, and only writes
  swreg101 when this param is 1 (R2). Fallback recovery: abort via swreg1, as in `vc8k_hw.c`.
- `ma35d1_pp_reg91_zero` (default 1): write swreg91 = 0 in `.init` and before any PP job (dormant
  for NV12).
- Exit: params visible in `/sys/module/hantro_vpu/parameters/`.

### V3 — Streams and reference checksums (host)
Generate in `vpu/videos/` with `gen_streams.sh` (ffmpeg + libx264, testsrc2 pattern, Annex-B):

| Step | File | Features |
|---|---|---|
| S1 | `s1_bp_i_320x240.h264` | Baseline, CAVLC, I-only, 10 frames |
| S2 | `s2_bp_p_320x240.h264` | Baseline, P-frames, 1 ref, 30 frames |
| S3 | `s3_mp_cabac_640x480.h264` | Main, CABAC, 3 refs, 30 frames |
| S4 | `s4_hp_b_640x480.h264` | High, 8x8 transform, B-frames + reordering, 30 frames |
| S5 | `s5_hp_b_1280x720.h264` | S4 at 720p, 60 frames |
| S6 (opt) | `bubles_1080p_base.264` copy | real 1080p clip |

For each: `ffmpeg -i X -f rawvideo -pix_fmt nv12 X.nv12` and per-frame md5 list `X.md5`
(`-f framemd5` or split + md5sum). Widths are multiples of 16 so no crop/stride ambiguity.

### V4 — Decode on target, compare (pass criterion D3)
- Copy `vpu/videos/` to `/mnt/vivante2d/videos/`.
- Decode:
  `gst-launch-1.0 filesrc location=S.h264 ! h264parse ! v4l2slh264dec ! video/x-raw,format=NV12 ! filesink location=S.out.nv12`
- Compare with `cmp_frames.sh` (split by frame size, md5 per frame vs `S.md5`, print first
  mismatching frame). Host or target, whichever has the tools.
- Order S1 -> S5, stop at first failure and debug (register dump via `hantro` debug + `devmem`).
- Exit: S1..S5 bit-exact. Record fps for S5 (`fpsdisplaysink video-sink=fakesink` or `time`).

### V5 — Display (last)
- `gst-launch-1.0 filesrc ! h264parse ! v4l2slh264dec ! waylandsink` (Weston) or
  `kmssink` (with Weston stopped), NV12 plane.
- Exit: visually correct playback of S5/S6.

### Later (not in minimal)
- ARGB/XRGB via PP RGB output (extend `hantro_postproc.c` for G1 RGB channel setup).
- MPEG-2 / VP8 codec ops (mainline already has them).
- Runtime PM tuning, reserved-memory size for 1080p with many refs.

## 5. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Register layout differs from G1 in fields mainline writes | Bit-exact S1..S5 tells us; diff swreg map v6 vs `hantro_g1_regs.h` on failure |
| R2 | swreg101 soft-reset undocumented on NanoD | Not written by default (`ma35d1_soft_reset=0`) |
| R3 | NanoD needs per-ref chroma bases (swreg102–118) | Vendor SW does not use them; check if S2+ fail |
| R4 | 32 MiB pool too small at 720p/1080p with DPB + MVs | Enlarge `vc8k_buf` (allowed, D6) |
| R5 | `v4l2slh264dec` requires features G1 lacks (e.g. interlaced) | Streams are progressive |
| R6 | ID `0x6e645000` not a G1 ID | Mainline does not check IDs |

## 6. Device tree change (hint)

`$K/arch/arm64/boot/dts/nuvoton/ma35d1.dtsi` (or board dts overriding it):

```dts
vc8k: vc8k@40290000 {
	compatible = "nuvoton,ma35d1-vpu";      /* was "nuvoton,ma35d1-vc8k" (DRM) */
	reg = <0x0 0x40290000 0x0 0x1000>;
	clocks = <&clk VC8K_GATE>;
	clock-names = "vpu";                     /* new, Hantro needs names */
	interrupts = <GIC_SPI 28 IRQ_TYPE_LEVEL_HIGH>;
	memory-region = <&vc8k_buf>;             /* keep */
	status = "okay";
};
```

To go back to the DRM driver, change only the compatible string back.

## 7. Configs to enable

### Kernel (`make linux-menuconfig`)
| Symbol | Value | Where |
|---|---|---|
| `CONFIG_MEDIA_SUPPORT` | y (already) | Device Drivers -> Multimedia support |
| `CONFIG_MEDIA_PLATFORM_SUPPORT` | y | Multimedia support -> Media device types -> Platform-specific devices |
| `CONFIG_V4L_MEM2MEM_DRIVERS` | y | Media drivers -> Media platform devices -> Memory-to-memory |
| `CONFIG_MEDIA_CONTROLLER` | y (already) | |
| `CONFIG_MEDIA_CONTROLLER_REQUEST_API` | selected by Hantro | |
| `CONFIG_VIDEO_HANTRO` | y (or m) | Media platform drivers -> Verisilicon -> Hantro VPU driver (visible after the Kconfig patch) |
| `CONFIG_VIDEO_HANTRO_MA35D1` | y | same menu (new) |
| `CONFIG_DRM_VC8K` | y/m (keep) | binds only with the vc8k compatible |

Save with `make linux-update-defconfig` (or your usual defconfig flow).

### Buildroot (`make menuconfig`)
| Symbol | Status |
|---|---|
| `BR2_PACKAGE_GSTREAMER1` | already y |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_V4L2CODECS` | y (done) |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_VIDEOPARSERS` | y (done) — `h264parse` |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_KMS` | y (done) — `kmssink` |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_WAYLAND` | already y |
| `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_VIDEOCONVERTSCALE` | already y |
| `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_VIDEOTESTSRC` | optional |
| `BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_ISOMP4` | already y (mp4 input) |
| `BR2_PACKAGE_LIBV4L` + `BR2_PACKAGE_LIBV4L_UTILS` | y (done) — `v4l2-ctl`, `media-ctl` |
| `BR2_PACKAGE_FFMPEG` | y (done) — SW reference only; no v4l2-request in this ffmpeg |
| `BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_V4L2` | not needed (stateful) |

## 8. Files

```
vpu/v4l2/
  plan.md                this file
  sync.sh                kernel/ -> $K/drivers/media/platform/verisilicon/
  kernel/                full mirror of verisilicon/ + ma35d1_vpu_hw.c
  tests/gen_streams.sh   host: S1..S5 + reference md5 (writes ../videos)
  tests/cmp_frames.sh    per-frame md5 compare
  tests/run_v4.sh        target: decode S1..S5 and report pass/fail
vpu/videos/              generated streams + references
```
