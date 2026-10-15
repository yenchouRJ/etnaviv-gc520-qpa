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

## 2. Stack (bottom to top)

```
L7  gst-launch-1.0 / app                          filesrc ! h264parse ! v4l2slh264dec ! sink
L6  gst1-plugins-bad v4l2codecs (1.22.9)          parses SPS/PPS/slices, builds DPB + ref lists
--- /dev/videoN + /dev/mediaN (V4L2 stateless H.264 controls + media request API) ---
L5  V4L2 core: vb2-dma-contig, v4l2-mem2mem, request API, v4l2-h264 helpers        (mainline)
L4  hantro_drv.c / hantro_v4l2.c                                                    (mainline + match entry)
L3  hantro_g1_h264_dec.c, hantro_h264.c, hantro_g1.c (irq/reset)                   (mainline)
L2  ma35d1_vpu_hw.c: fmts, clk, irq, ID log                                     (NEW)
L1  VC8000NanoD @ 0x40290000, SPI 28, VC8K_GATE clock, vc8k_buf pool
```

## 3. Facts checked in the tree (6.6.93)

- `VIDEO_HANTRO` depends on `ARCH_MXC || ARCH_ROCKCHIP || ARCH_AT91 || ARCH_SUNXI || COMPILE_TEST`;
  needs `ARCH_MA35D1` added. Current kernel `.config`: `VIDEO_HANTRO` off, `V4L_MEM2MEM_DRIVERS` off,
  `MEDIA_CONTROLLER=y`, `CONFIG_ARCH_MA35D1=y`.
- Probe (`hantro_drv.c`): clocks via `devm_clk_bulk_get` (names required), reg index 0 if no
  `reg_names`, IRQ index 0 if irq name is NULL, resets optional, sets 32-bit coherent DMA mask.
- `hantro_g1_reset()` writes `G1_REG_SOFT_RESET` (swreg101, 0x194), which the NanoD register map
  does not list. Our reset wrapper only writes it if `ma35d1_soft_reset=1` (default 0).
- NanoD has extra registers (swreg102–118, per-reference chroma bases) that mainline never writes.
  Vendor SW does not use them either (`DEC_X170_REGISTERS 60`, `PP_X170_REGISTERS 41`). Not needed in practice (R3 closed).
- Mainline PP outputs only YUYV (`VPU_PP_OUT_RGB` defined but unused). RGB is a later phase.

## 4. Phases

### V0 — Configs and tree sync  [DONE]
- Write `sync.sh` (rsync `kernel/` -> `$K/drivers/media/platform/verisilicon/`, with `--dry-run`
  option and a diff summary). Never deletes files outside that directory.
- User enables configs (section 7), rebuilds kernel and rootfs.
- Exit: kernel builds with `VIDEO_HANTRO=y`, `v4l2-ctl`/`media-ctl`/`gst-inspect-1.0 v4l2codecs`
  present on target.

### V1 — Variant + probe  [DONE]
- `ma35d1_vpu_hw.c`:
  - `ma35d1_vpu_fmts[]`: `V4L2_PIX_FMT_NV12` (raw) + `V4L2_PIX_FMT_H264_SLICE` (coded), frmsize
    48..1920 x 48..1088 step 16 (from the HW features doc, capped to 1080p).
  - `codec_ops[HANTRO_MODE_H264_DEC] = { .run = hantro_g1_h264_dec_run, .reset = ma35d1_vpu_reset,
    .init = hantro_h264_dec_init, .exit = hantro_h264_dec_exit }`.
  - irqs `{ NULL, hantro_g1_irq }` (unnamed, index 0), `clk_names = { "vpu" }`.
  - `.run` wrapper logs swreg0 (ID) and swreg2 once at the first job.
  - `.reset` = IRQ disable + clock gate; swreg101 soft reset only with `ma35d1_soft_reset=1`.
- `hantro_drv.c`: add `{ .compatible = "nuvoton,ma35d1-vpu", .data = &ma35d1_vpu_variant }`
  under `#ifdef CONFIG_VIDEO_HANTRO_MA35D1`.
- `hantro_hw.h`: `extern const struct hantro_variant ma35d1_vpu_variant;`
- `Kconfig`: add `ARCH_MA35D1` to `VIDEO_HANTRO` depends, new `VIDEO_HANTRO_MA35D1` (bool, default y).
- `Makefile`: `hantro-vpu-$(CONFIG_VIDEO_HANTRO_MA35D1) += ma35d1_vpu_hw.o`.
- DT: change compatible, add `clock-names = "vpu"` (section 6).
- Exit: `dmesg` shows hantro probe on 40290000, `/dev/video0` + `/dev/media0` exist,
  `v4l2-ctl --list-formats-out` shows `S264`, `--list-formats` shows `NV12`.

### V2 — Streams and reference checksums (host)  [DONE]
Generate in `vpu/videos/` with `gen_streams.sh` (ffmpeg + libx264, testsrc2 pattern, Annex-B):

| Step | File | Features |
|---|---|---|
| S1 | `s1_bp_i_320x240.h264` | Baseline, CAVLC, I-only, 10 frames |
| S2 | `s2_bp_p_320x240.h264` | Baseline, P-frames, 1 ref, 30 frames |
| S3 | `s3_mp_cabac_640x480.h264` | Main, CABAC, 3 refs, 30 frames |
| S4 | `s4_hp_b_640x480.h264` | High, 8x8 transform, B-frames + reordering, 30 frames |
| S5 | `s5_hp_b_1280x720.h264` | S4 at 720p, 60 frames |
| S6 | `s6_bp_1080p.h264` | Constrained Baseline 1080p, 120 frames, real clip (compared in tight I420: 1080 lines decode as 1088 + crop) |
| S7 | `s7_hp_b_1024x600.h264` | High, B-frames, 150 frames at panel size (display test only, no reference) |
| S6 (opt) | `bubles_1080p_base.264` copy | real 1080p clip |

For each: `ffmpeg -i X -f rawvideo -pix_fmt nv12 X.nv12` and per-frame md5 list `X.md5`
(`dd` + md5sum). Widths and heights are multiples of 16, so no crop/stride ambiguity.
S6 (real 1080p) PASS, see V3 result.

### V3 — Decode on target, compare (pass criterion D3)  [DONE]
- Scripts in `tests/` use only busybox tools (no `timeout`, `split`, `stat` on target).
  Copy `tests/*.sh` and `videos/*.{h264,md5,info}` to `/mnt/vc8k/{tests,videos}/`.
- `./tests/run_v4.sh /mnt/vc8k/videos [streams...]`: decodes through
  `filesrc ! h264parse ! v4l2slh264dec ! video/x-raw,format=NV12 ! filesink` (output in
  `/mnt/vc8k/out`; `/tmp` is a small tmpfs), kills gst after 240 s (`TMO`), compares per-frame md5.
- Hantro capture buffers carry H.264 motion-vector data after the picture, so `filesink`
  writes more than the visible frame (e.g. 134432 B vs 115200 B at 320x240). `cmp_frames.sh`
  derives the per-buffer pitch from file size / frame count and checks the first visible bytes.
- Result (S1..S6): all PASS bit-exact (10/10, 30/30, 30/30, 30/30, 60/60, 120/120 frames), no
  timeouts or kernel warnings. S5 (720p, B-frames) decodes 60 frames in about 1.06 s to `fakesink`
  (~55-60 fps including start-up, unsynced). S6 (real 1080p, Constrained Baseline) decodes 120
  frames in 4.28 s (~28 fps, unsynced). The 32 MiB pool was enough.
- S6 is compared as tight I420 (1080 lines decode as 1088 + crop, so the NV12 buffer is padded).
  The software conversion reads write-combined buffers and takes 66-75 s, hence `TMO=240`.
- Frame-size limit: the variant first used mainline's `FMT_HD_*` (max 1280x720), so 1080p failed
  in `VIDIOC_S_FMT`. Now `FMT_FHD_*` (1920x1088).

### V4 — Display  [DONE: NV12 zero-copy on the primary plane]
Panel 1024x600, display driver `ma35-drm`. Planes: 31 primary, 38 and 49 overlay.

**Zero-copy path (kmssink).** Stop Weston first (otherwise `drmModeSetPlane` returns EACCES;
busybox has no `pkill`, and `/etc/profile.d/weston.sh` restarts Weston on login):
```cmd
kill -9 $(pidof weston)
gst-launch-1.0 filesrc location=/mnt/vc8k/videos/s7_hp_b_1024x600.h264 ! h264parse ! v4l2slh264dec ! kmssink driver-name=ma35-drm plane-id=31
```
Result: S7 (150 frames) runs for exactly 5.0 s (30 fps), no GStreamer or kernel errors.

- **Display-driver fix:** `ma35_drm.c` used `DRM_GEM_DMA_DRIVER_OPS_VMAP_WITH_DUMB_CREATE`. Its
  PRIME import calls `dma_buf_vmap()`, which fails on Hantro buffers (`DMA_ATTR_NO_KERNEL_MAPPING`).
  Now `DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE`.
- **Primary plane (31):** full-screen only (`can_position=false`, `DRM_PLANE_NO_SCALING`), so the
  frame must be exactly 1024x600. Other sizes give `drmModeSetPlane: Invalid argument`.
- **Overlay plane (38, kmssink default):** imports and is active in
  `/sys/kernel/debug/dri/0/state`, but nothing is visible without an enabled primary plane.
- Not done: other sizes without Weston (needs a primary background, or scaling or positioning on
  the primary plane; display-driver policy).

**Weston path (CPU conversion, fallback).** Weston uses pixman, which rejects NV12 in
`waylandsink`:
```cmd
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
gst-launch-1.0 filesrc location=S.h264 ! h264parse ! v4l2slh264dec ! videoconvert ! videoscale ! video/x-raw,width=1024,height=576,pixel-aspect-ratio=1/1 ! waylandsink
```
The picture is visible, but frames are dropped (CPU-bound), and the kiosk shell kills clients
whose surface exceeds the screen.

### Later (not in minimal)
- ARGB/XRGB via PP RGB output (extend `hantro_postproc.c` for G1 RGB channel setup).
- MPEG-2 / VP8 codec ops (mainline already has them).
- Runtime PM tuning, reserved-memory size for 1080p with many refs.

## 5. Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Register layout differs from G1 in fields mainline writes | CLOSED: S1..S6 bit-exact |
| R2 | swreg101 soft-reset undocumented on NanoD | Not written by default; error-recovery path not yet exercised |
| R3 | NanoD needs per-ref chroma bases (swreg102-118) | CLOSED: not needed, S2..S6 pass |
| R4 | 32 MiB pool too small at 720p/1080p with DPB + MVs | CLOSED for Baseline 1080p (S6 passes); recheck with High-profile 1080p and many refs |
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
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_KMS` | y (done) — `kmssink` (works, see V4) |
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
  tests/gen_streams.sh   host: S1..S6 + reference md5 (writes ../videos)
  tests/cmp_frames.sh    per-frame md5 compare
  tests/run_v4.sh        target: decode S1..S6 and report pass/fail
vpu/videos/              generated streams + references
```
