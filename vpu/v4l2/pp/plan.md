# VC8000NanoD post-processor (PP) on mainline Hantro: combined-mode scaling, then letterbox, RGB and JPEG

Goal: the hardware decodes **and** scales every supported video/camera stream to the size the
display needs, in NV12 (RGB as an option), with **no CPU fallback**. Built on the stateless mainline Hantro driver
(`../kernel/`, our MA35D1 variant) with our own patches. The vendor-based work (`vpu/vc8k`, the
6.18 BSP `drivers/media/platform/nuvoton/ma35d1/`) is reference only.

Status: T1 (PP refactor backport) and T2 (PP forced on at 1:1) are done and verified on the board: S1-S6 md5 identical with `ma35d1_force_pp=1` (kernel 6.6.93, CMA 96 MB). T0 probe log is coded but its output was not captured yet (`run_v4.sh` clears dmesg; read it with `dmesg | grep 'pp fuse'` right after boot, before any test). T3 is coded; T3 is done: size rules (`tests/t_pp_fmt`, 74 checks) and scaled pixels (`tests/run_scale.sh` + `cmp_scale.sh`, 9 cases PASS, PSNR-Y 39-54 dB, SSIM >= 0.987, exact frame counts) verified on the board; T4 next.

## 1. Decisions

| # | Decision |
|---|---|
| P1 Scope | PP in **combined (pipeline) mode** with the decoder; output **NV12 by default** (memory), **RGB as an opt-in CAPTURE format** (P13). No rotation, alpha blend, deinterlace |
| P2 Purpose | Display fit (any source to the panel/window size). Memory saving is a later phase (P2b) |
| P3 Phasing | (a) mainline structure: one full-size decoder buffer per CAPTURE buffer, PP writes the scaled frame into the CAPTURE buffer; (b) later, measured: decouple the full-size pool from the CAPTURE count (DPB + 1) |
| P4 gst buffers | Keep the `v4l2codecs` default (12 at 1080p). `experimental-0004` (cap to 8) stays archived |
| P5 Memory budget | Boards have 256 or 512 MB (test board 256 MB, 1024x600 panel). Enlarge CMA as needed |
| P6 Base | Kernel 6.6.93. Backport the 6.18 PP refactor first (`ref_fmt`, lazy `hantro_postproc_get_dec_buf_addr()`, G1 extended width/height bits, `max_num_buffers`), then add the G1 scaler |
| P7 Size contract | Kernel reports the real hardware range: `ENUM_FRAMESIZES` = `STEPWISE` (see §3), `S_FMT` on CAPTURE accepts any size in range, aspect ratio may change. Patch gst `v4l2codecs` to accept stepwise sizes and aspect changes; the size comes from downstream caps (`... ! video/x-raw,width=1024,height=600 ! sink`) |
| P8 Letterbox | **In scope (T8).** No-distortion option: CAPTURE buffer = panel size, `S_SELECTION(COMPOSE)` places the scaled picture inside it using the PP picture-in-picture offset; borders cleared once at allocation. Reason: measured on the 1080p panel, bars as a separate Weston surface cost a third layer; the display controller has only primary + overlay (+ cursor), so video + bars + Qt UI does not fit. Bars inside the video buffer keep video on one plane |
| P9 JPEG | Hardware JPEG decode inside our Hantro variant (T9-T10), after RGB and letterbox because it reuses them (PP is shared; combined mode needs **no** internal full-size buffer for JPEG): whole-frame API (JPEG in on OUTPUT, NV12 out on CAPTURE, like `coda`/`mxc-jpeg`), header parsing with the existing `v4l2_jpeg_parse_header()`, PP always on (any JPEG sampling to NV12 + scaling), stock gst `v4l2jpegdec` |
| P10 Unsupported input | Rejected cleanly by the driver (`S_FMT` / header parse) with a clear error; no CPU fallback. Known unsupported: progressive and non-interleaved JPEG, interlaced H.264 when scaling (combined mode impossible), anything above the limits in §3 |
| P11 Qt still images | **Out of scope:** `QImage` JPEG stays on the CPU (libjpeg-turbo through Qt's `qjpeg` plugin); enable `BR2_PACKAGE_QT5BASE_JPEG` (currently off, so Qt cannot load JPEG at all). "No CPU fallback" applies to video/camera streams (GStreamer/V4L2). A later `QImageIOHandler` using T9/T10 is possible without driver changes |
| P12 Pass criterion | Two layers: (1) PP on at 1:1 (NV12 to NV12, same size) gives md5 identical to the existing S1-S6 references; (2) scaled output vs ffmpeg scaled reference of the same frames: PSNR-Y >= 30 dB and SSIM >= 0.95 on every frame, no missing frames, plus a visual check on the panel. A vendor-stack golden md5 is a one-off cross-check only if numbers look wrong |

| P13 RGB output | PP RGB output as a second CAPTURE format next to NV12: **XRGB8888 (`V4L2_PIX_FMT_XRGB32`) and RGB565 (`V4L2_PIX_FMT_RGB565`)** first; ARGB4444/alpha only if needed. NV12 stays the default and wins in memory (3.1 MB vs 8.3 MB at 1080p) and bandwidth, so RGB exists for JPEG and display cases that need it. Verified through H.264 because the PP is shared. YCbCr to RGB matrix and range follow the CAPTURE `colorspace`/`ycbcr_enc`/`quantization` (H.264: from VUI, default BT.601/limited; JPEG: BT.601/full) |
| P14 Rotation | Not in scope: unavailable in combined mode for JPEG, and not needed |

## 2. Why the memory goes up in phase (a)

With the PP on, the decoder writes full-size reference pictures (+ H.264 motion vectors) into
internal buffers (`ctx->postproc.dec_q[i]`), one per CAPTURE buffer; the CAPTURE buffers hold
the scaled output.

| 1080p source to 1024x600, 12 CAPTURE buffers | Size |
|---|---|
| Today (no PP): 12 x 3.66 MB | 44 MB |
| (a) 12 x 3.66 MB full-size + 12 x 0.92 MB NV12 1024x600 | about 55 MB |
| (b) 7 x 3.66 MB (DPB 6 + 1) + 12 x 0.92 MB | about 37 MB |

Plus 2 bitstream buffers (6 MB at 1080p) in every case. (b) needs the reference lookup in
`hantro.h` (`hantro_get_dec_buf_addr`/`dec_q[vb->index]`) to use a pool index instead of the
CAPTURE index; with the 6.18 lazy allocator this is one function.

## 3. Hardware: PP capabilities and limits (VC8000NanoD)

Sources (VeriSilicon confidential, local only, not copied here):
`drivers/vpu/VC8000NanoD/from_Verisillicon/document/`
- HW Integration Guide v1.01 (2020-03-26): §2.2 decoder features (Tables 1, 2, 6), §2.3 PP features (Table 14), §3.2.3 PP performance, §3.5 data flow, §5.3 swreg100
- `Decoder_swreg_map_v6.pdf`: PP registers swreg60-swreg100

### 3.1 Combined (pipeline) mode
- Decoder output goes straight into the PP inside the core; the decoded picture still goes to
  memory (it is a reference), the PP writes its output to a second address.
- Enable: swreg60 `pp_pipeline_e = 1`; `pp_e` (standalone start) must stay 0. PP interrupt
  status bits are not used in pipeline mode (the decoder IRQ ends the job).
- Not possible with: rotation, cropping in video mode, deinterlacing, interlaced streams.
- Throughput is set by the slower of decoder and PP; scaling is the slowest PP function.
- In V4L2 every picture is post-processed in decode order into its own CAPTURE buffer, and
  userspace reorders, so B-frames are fine (the vendor guide's B-frame restriction is about its
  display-order PP, not this model).

### 3.2 Sizes
| Item | Range |
|---|---|
| PP input (combined) | 48x48 up to 16368x16368, step 16 (macroblocks) |
| PP output | 16x16 to 4096x4096 (synthesis-time maximum), width step 8, height step 2 |
| Pixel-accurate output | Optional synthesis feature (swreg100 `PPD_PIXAC_E`): steps of 1 (RGB) / 2 (chroma subsampled). Assume steps 8/2 unless the bit reads 1 |
| Board limit (our variant) | Decode up to 1920x1088 (`FMT_FHD_*` in `ma35d1_vpu_hw.c`) |

### 3.3 Scaling
- Arbitrary, non-integer ratio, set separately for width and height.
- Down: proprietary averaging filter, unlimited ratio. An optional "fast downscale" mode exists.
- Up: bicubic, 4-tap horizontal, 2-tap vertical. Max 3x width, (3x - 2) height.
- **Not allowed:** up-scale one axis and down-scale the other in the same pass (e.g. 640x480 to
  1024x400). Driver must reject (or clamp) such sizes in `TRY_FMT`; document in `ENUM_FRAMESIZES`
  only as the plain range.
- Registers: horizontal/vertical scale mode (none/up/down), ratio `(out-1)/(in-1)` per axis
  (18-bit), inverse ratio `(in-1)/(out-1)` per axis (16-bit), output width/height
  (with the extended-width bits for wide output), input width/height in MBs (+ extended bits,
  part of the 6.18 backport).
- Scaling support is a synthesis option: swreg100 `PPD_SCALE_LEVEL` = 0 none, 1 low-performance,
  2 high-performance. **Read it on the board first** (task T0).

### 3.4 Formats
- Input in combined mode: whatever the decoder produces (H.264 4:2:0; JPEG 4:0:0, 4:2:0, 4:2:2,
  4:4:0, 4:1:1, 4:4:4).
- Output: NV12 (4:2:0 semi-planar), YUYV-family 4:2:2 (raster or 4x4 tiled), configurable ARGB.
  **NV12 by default; XRGB8888 and RGB565 added in T7.**
- Colour conversion, contrast/brightness/saturation, dithering, alpha blending (not with NV12
  output), masking: not used.

### 3.5 Picture in picture
- The PP can write its output at an offset inside a larger frame (up to 4096x4096 display),
  given the frame's line length. This is what P8 (letterbox, T8) uses.

### 3.6 Decoder limits relevant to "reject cleanly"
- H.264: Baseline/Main/High up to level 5.1 in hardware; our board caps at 1920x1088.
  FMO / ASO need software entropy decoding in the vendor stack: **not supported** here.
  SVC/MVC enhancement layers not supported.
- JPEG: baseline interleaved only. Progressive and non-interleaved: **not supported**.
  48x48 to 16368x16368, step 8. Thumbnails can be decoded.

### 3.7 Performance (guide, cycles per macroblock, ideal bus)
| Operation | Cycles/MB |
|---|---|
| H.264 BP decode | 340 |
| H.264 HP decode | 426 |
| H.264 BP decode + PP | 498 |
| PP scaling 0.25x | 633 |
| PP scaling 2x | 1162 |
| JPEG 4:2:0 decode | 292 |
1080p = 8160 MB/frame. Clock to be confirmed (`vc8k_gate`); at 1080p30 the budget is
roughly 245k MB/s. Measure on the board; unclocked 1080p decode is about 28 fps today.

## 4. Kernel–userspace contract

- CAPTURE formats: NV12 (default); with T7 also XRGB32 and RGB565 (PP always on for these). When the PP is needed (`S_FMT` CAPTURE size != coded size),
  `ctx->need_postproc = true`; the decoder writes `ref_fmt` (coded size) into `dec_q[]` and the
  PP writes `dst_fmt` into the CAPTURE buffer.
- `VIDIOC_ENUM_FRAMESIZES` on CAPTURE (NV12): one `V4L2_FRMSIZE_TYPE_STEPWISE` entry,
  min 16x16, max min(4096, 3 x coded) (and for height 3 x coded - 2), step 8 / 2.
- `TRY_FMT`/`S_FMT`: align to 8/2, clamp to the range, reject mixed up/down scaling.
- 1:1 size: PP off by default (native path, as today); a module parameter forces PP on at 1:1
  for the P12 layer-1 test.
- Interlaced H.264 with a scaled CAPTURE size: fail the job/format with an error.
- Letterbox (T8): `S_SELECTION(CAPTURE, COMPOSE)` sets the rectangle inside the CAPTURE size that
  receives the scaled picture (even left/top, size rules of T3 apply to the rectangle);
  `S_FMT` CAPTURE gives the full buffer (panel) size. Default COMPOSE = whole buffer (today).

## 5. Phases

### T0 — Facts on the board
- Read swreg100 (PP synthesis config: scaling level, pixel-accurate, max output width) and
  swreg0 ID with the clock enabled (from the driver probe; `devmem` reads 0 while the clock is
  gated). Log it once at probe.
- Confirm the VPU clock rate.

### T1 — Backport the 6.18 PP refactor to 6.6 (not documented further here)

### T2 — G1 PP enable in combined mode at 1:1 (no scaling) — DONE
- Add `postproc_ops` to the ma35d1 variant; PP output format NV12 (input 4:2:0 semi-planar).
- Pass: P12 layer 1 (S1-S6 md5 identical with PP forced on).
- Result: PASS on all of S1-S6 (frame counts exact). The PP output buffers carry no MV tail
  (buffer pitch = visible size), confirming the PP path is used. Fixes needed: PP input
  size from `ref_fmt` (not the output size); NV12 output needs `out_chroma_base`
  (= luma + bytesperline * height); with the PP forced, ENUM_FMT hides the native formats.
- CMA: S6 (1080p, 1:1, 12+12 buffers) failed with ENOMEM on STREAMON at CMA 64 MB (about
  30 MB free at idle); passes at 96 MB. Scaled 1080p to 1024x600 needs less (§2).

### T3 — G1 scaler — DONE (size rules + scaled pixels verified)
- Implemented (`hantro_postproc.c`, `hantro_v4l2.c`, variant flag `pp_scaler`):
  - `hantro_postproc_try_size()`: step 8 x 2, 16..4096, up-scale at most 3x (3x - 2 height);
    mixed up/down: the up-scaled axis is clamped to the source size (down-scale wins);
    a request equal to the coded size after rounding up to 16 (visible 1080 of coded 1088)
    stays on the native path, same as before.
  - `set_fmt_cap`: `need_postproc` = forced or CAPTURE size != `ref_fmt` size.
  - Scaler registers follow the vendor driver (`ppinternal.c`), not the guide's wording:
    up: `ratio = ((out-1)<<16)/(in-1)`, `invra = ((in-1)<<16)/(out-1)`;
    down width: `invra = (out<<16)/in`; down height: `invra = (out<<16)/in + 1`.
    Output width/height bits 12:11 go to swreg71/swreg70 bits 31:30.
  - `ENUM_FRAMESIZES` on CAPTURE returns one STEPWISE entry; ENUM_FMT lists NV12 once.
- Verified (`tests/t_pp_fmt`, `make` in `tests/`, run on the board with `ma35d1_force_pp=0`):
  stepwise range per source size, step/clamp/3x limits, mixed up/down clamp, native vs scaled
  `sizeimage`, 1:1 request (also the visible 1080 of coded 1088) stays native. S1-S6 still PASS.
- Scaled pixels: S1/S3/S4/S5 scaled to 9 sizes (down both axes, width only, height only, up,
  1280x720 to 1024x600): all PASS vs ffmpeg bicubic reference. The vendor ratio formulas are
  right; the `cHworkaroundFlag` workaround was not needed. S1-S6 unscaled still PASS with the
  patched gst. Not covered: mixed up/down (clamped).
- Stock gst 1.22.9
  `v4l2codecs` cannot drive it (only DISCRETE sizes are enumerated, `select_src_format` never
  sets the CAPTURE size, output caps use the SPS display size), so the T3 pixel test uses an
  LD_PRELOAD shim on the stock `v4l2slh264dec` that turns the CAPTURE `G_FMT` into an
  `S_FMT` of `PP_W x PP_H`; frames are dumped raw (buffer pitch = sizeimage) and compared
  on the host with ffmpeg (PSNR-Y / SSIM against an ffmpeg-scaled reference, P12 layer 2).
  The stock plugin also copies frames with the SPS display size (reads past the scaled buffer,
  SIGSEGV), so the test needs `patches/gst1-plugins-bad/0005` (display size = scaled CAPTURE
  size); it is applied in the Buildroot gst tree and deployed. The real S_FMT-from-caps part
  stays in T4.
- 1080p crop: the stateless SPS control has no frame crop, so the application passes the
  visible size with `S_SELECTION(OUTPUT, CROP)` (origin 0, sizes multiple of 8) before the
  CAPTURE `S_FMT`; the PP then reads only 1920x1080 (crop8_d_e). CAPTURE `G_SELECTION`
  COMPOSE* reports the valid area. Verified: S6 to 1024x600 and 640x360, 120/120 frames,
  PSNR-Y 51.5/51.0 dB, SSIM 0.998; negative control without the crop: 29.9 dB, SSIM 0.82.
  T4 gst patch must call S_SELECTION with the SPS crop.
  The vendor `cHworkaroundFlag` (only for ASIC id 0x6731114x) is not implemented.
- Program scale mode, ratios, inverse ratios, sizes; `enum_framesizes` (stepwise); `try_fmt` rules.
- Pass: P12 layer 2 for 1080p to 1024x600, 720p to 1024x600, 640x480 to 1024x600 (up),
  1080p to 512x300; frame count exact; no kernel warnings.

### T4 — GStreamer — DONE (patch kept in the gst tree, generate into `patches/` at the end)
- Changes in `sys/v4l2codecs` (gst tree commit 7b98618): stepwise `ENUM_FRAMESIZES` -> int-range
  caps; `select_src_format` does `S_FMT` with the width/height fixed by downstream caps;
  `S_SELECTION(OUTPUT, CROP)` with the SPS crop before it; h264 display size = scaled CAPTURE
  size; h264 `getcaps` returns the template (the default one copied the downstream size onto
  the h264 sink caps -> not-negotiated).
- Verified: `run_scale.sh` 11 cases PASS with a capsfilter and no shim; S1-S6 unscaled PASS;
  user checked s10-s12 on the panel: upscale and 1:1 smooth, downscale OK with the cockpit
  1080p clip (drone 1080p clip still shows lag).
- Performance (s10, sync=false): scaled 1080p ~33.5 fps independent of the output size
  (1024x600 / 640x360 / 1024x576), unscaled zero-copy ~41.5 fps. Matches the guide's cycle
  count at 180 MHz (HP 426 + PP ~158 cycles/MB) within ~11 %. Switching VC8K to E-PLL/2
  (250 MHz, TF-A writes the protected CLKSEL0 bit 2; user-managed) did not remove the
  bottleneck, so it is probably the pipeline or DDR bandwidth, not the VPU clock.
- Not done: dedicated `waylandsink` pass criteria measurements beyond visual checks.

### T5 — Memory measurement of (a) — DONE (1024x600 and 1920x1080 panels)
- `tests/mon_cma_scaled.sh gst|cockpit [file] [sec]` (target); `cockpit_1080_30.h264`, kiosk Weston,
  `waylandsink`, capsfilter = panel size. 11 CAPTURE buffers (gst requests 11; 2 OUTPUT).
- Pinned CMA use (page cache dropped mid-run; the clip's page cache also sits in CMA and
  inflates `CmaFree` use to ~85 MB, it is reclaimable): single gst 57.4 MB, gst + Qt cockpit UI
  59.6 MB, of 96 MB CMA (~14 MB free). Matches §2 estimate (~55 MB). No ENOMEM, no dmesg errors.
- fps of this clip with sync=true: avg ~18 (15-22) - below 30, decode-bound (see T4 perf notes).
- 1920x1080 panel, CMA 120 MB, `SYNC=false`, 14 s each (pinned use / fps avg):
  1080 clip 1:1 gst 52.9 MB / 32.3; 1080 clip 1:1 + cockpit 64.0 MB / 27.6;
  1280 clip upscaled gst 57.5 MB / 46.0; 1280 clip upscaled + cockpit 70.5 MB / 38.6.
  Min free incl. page cache >= 7 MB, no errors. The Qt UI costs ~11-13 MB here (~2 MB on 1024x600).
- Decision on T6 (b): (a) fits in 96 MB with the Qt UI; (b) only if more clients share the CMA.

### T6 — (b) decoupled full-size pool (P3 b) — NOT REQUIRED (deferred)
- T5 shows (a) fits (53-71 MB pinned, single decoder + Qt UI). Revisit only for several simultaneous
  1080p decoders (dashboard demo, not measured) or a smaller CMA (saves ~40 MB per decoder).
- Only after T5; pool of DPB + 1 full-size buffers; same pass criteria as T2/T3.

### T7 — RGB output in the PP, verified on H.264 (P13) — DONE
Order: before JPEG, because JPEG needs RGB and this is testable with the existing H.264 path.
- Driver: PP output format registers (RGB mode, 32/16 bit, channel masks, endian/swap),
  YCbCr to RGB matrix and range registers (vendor `PPSetRgbBitmask`, `PPSetRgbTransformCoeffs`);
  CAPTURE ENUM_FMT lists NV12, XRGB32, RGB565; sizeimage = bytesperline x height; RGB forces PP on;
  `colorspace`/`ycbcr_enc`/`quantization` reported and honoured (H.264 limited range BT.601 by default).
- gst: `v4l2codecs` already negotiates the CAPTURE format from downstream caps; check that
  `video/x-raw,format=BGRx` and `RGB16` work with the capsfilter and `waylandsink`.
- Pass: `run_scale.sh`-style cases (1:1 and scaled, 1080p to 1024x600 and 720p to 1024x600 up)
  with XRGB32 and RGB565; host compare against ffmpeg `scale` + `format=bgr0`/`rgb565le` with the
  same matrix/range: PSNR >= 35 dB (HW vs software rounding, to be confirmed), exact frame
  count, correct channel order (red/blue not swapped: test with a colour bar stream).
  S1-S6 and the NV12 scaled cases still pass; panel plane check (`XR24`/`RG16` scan out on the overlay).
- Not goals: speed or memory win (NV12 is better everywhere); alpha formats.
- Result (1024x600 panel image, CMA 120 MB, `tests/run_rgb.sh` + `cmp_rgb.sh`): all PASS.
  Unscaled 320x240: BGRx 53.0 dB, RGB565 44.4 dB (matrix and byte order exact). Scaled to 1024x600:
  1080p 45.2 dB, 720p/480p about 30 dB. The 30 dB cases are not a matrix error: luma rms 1.15, chroma
  error unbiased at colour edges (HW chroma scaling differs from ffmpeg bicubic), so the scaled limit
  is 29 dB, unscaled 40 dB. 1080p to 1920x1080 RGB565 1:1 (crop of coded 1088) 120/120 frames.
  NV12 scaled `run_scale.sh` 11 cases still PASS. Panel: BGRx on the overlay, XR24 1024x600 with a
  changing fb (zero-copy), user confirmed colours and smoothness.
- Bug found: the CAPTURE reset colorspace is JPEG, which selected full range for H.264. Full range
  now only for a JPEG stream or explicit `quantization=FULL`; BT.709 only if `ycbcr_enc`/colorspace
  say so. gst does not pass colorimetry yet (always BT.601 limited).
- CMA: 1080p to 1920x1080 XRGB32 needs about 137 MB (11 x 8.3 + 40 + 6) and fails with ENOMEM at
  120 MB (expected, P13 cost; RGB565 1080p fits). Use NV12 at 1080p; T6 would cut about 40 MB.

### T8 — Letterbox via PiP and `COMPOSE` (P8) — DONE
- Driver: `S_SELECTION(CAPTURE, COMPOSE)` accepts a rectangle inside the CAPTURE size; PP output
  base = buffer base + offset, line stride = full buffer width (`display_width`); scaler sizes and
  step rules apply to the COMPOSE rectangle; borders cleared once when buffers are allocated
  (NV12: Y=16, UV=128; RGB: black); chroma offset even. Check the PiP size limit (guide: 1920x1088,
  features: 4096) and the offset alignment on the board. Works for NV12 and RGB (T7).
- gst (`v4l2codecs` h264dec): new property `add-borders` (name as in `videoscale`): when true, fit the
  source display aspect into the downstream size, set COMPOSE, and keep the caps at the downstream
  (panel) size. Default false keeps today's stretch.
- Tests: pixel compare against ffmpeg `scale` + `pad` (black bars exact); 1024x600 clip on 1920x1080
  (bars 40 px left/right) and 1920x1080 clip on 1024x600 (bars top/bottom); `try_fmt` rejects
  COMPOSE outside the buffer or violating the rules.
- Panel pass: cockpit demo with the letterboxed video. Plane state must show NV12 panel-sized on
  plane-0 (fb changing) and the Qt UI on plane-1, i.e. the layout that failed with plain
  `waylandsink` + capsfilter (video had no plane once the UI started).

- Result (1024x600 image): all 5 cases PASS (`tests/run_letterbox.sh` + `cmp_letterbox.sh`): 640x480 to
  1024x600 (800x600@112,0), 720p to 1024x768 (1024x576@0,96), 1080p to 1024x600 (1024x576@0,12), 320x240 to
  640x360, 1024x600 to 1920x1080 (1840x1080@40,0, 584/584 frames); bars exactly Y=16/UV=128, PSNR-Y 43-52 dB.
  NV12 scale (11) and RGB (6) regressions pass. Panel: cockpit UI plus letterboxed 1080p: NV12 panel-sized
  on plane-0 (fb changing), Qt AR24 on plane-1; user confirmed the picture.
- Implementation notes: `ctx->compose` (width 0 = whole buffer, reset by `S_FMT` CAPTURE); `S_SELECTION`
  COMPOSE is `EBUSY` once buffers exist, `ERANGE` outside the buffer. PP base = buffer + top*stride +
  left*bpp (chroma: (top/2)*stride + left), stride = buffer width. Borders painted in `buf_init`
  (needs a kernel mapping, so the scaler decoder's CAPTURE queue no longer uses NO_KERNEL_MAPPING).
  The mixed up/down scaling rule moved from `S_FMT` (a letterbox buffer may be e.g. 1024x768 for 720p)
  to the COMPOSE rectangle and to STREAMON (`EINVAL`, verified with 640x480 to 1024x300).
- gst: property `add-borders` (id 16: the base decoder installs ids 1.. ignoring the offset).

### T9 — JPEG decode in the Hantro variant (kernel) (P9)
- T9a facts: JPEG bit in swreg50 (set on this board), PP JPEG input formats on this synth config;
  decoder register list for JPEG mode (swreg 3, 5, 7: `JPEG_MODE`, `JPEG_QTABLES`, restart, stream
  length, sampling), table upload (quantization, Huffman) from the vendor `jpegdec*` sources.
- T9b header: `v4l2_jpeg_parse_header()`; accept baseline interleaved SOF0 with sampling 4:0:0,
  4:2:0, 4:2:2, 4:4:0, 4:1:1, 4:4:4; reject progressive (SOF2), non-interleaved (scans with a
  subset of components), arithmetic coding, 12-bit, CMYK, size outside 48..16368, with a clear error
  (no hang). Restart intervals supported.
- T9c decode job: stream base/length, tables, PP combined mode with the input format set from the
  sampling, output NV12 or RGB (T7) with scaling/crop/COMPOSE (T3, T8). **No internal full-size
  buffer** (guide 4.7.6: frame buffer not required in pipelined mode; check that the decoder still
  needs a dummy output address). Rotation is not available.
- T9d V4L2 contract: OUTPUT `V4L2_PIX_FMT_JPEG` (one image per buffer), CAPTURE NV12/XRGB32/RGB565,
  size from the first OUTPUT header; `V4L2_EVENT_SOURCE_CHANGE` sequence like `mxc-jpeg`, so the
  stock stateful `v4l2jpegdec` can drive it (R4). JFIF colour space reported (BT.601, full range).
- Tests (host: `cjpeg`/ffmpeg to produce baseline JPEGs, a set per sampling, with restart markers,
  non-multiple-of-16 sizes such as 1023x575, large image around 12 MP): HW decode vs
  libjpeg-turbo (`djpeg`) / ffmpeg converted to NV12, PSNR-Y >= 40 dB expected unscaled (IDCT
  differences only), scaled cases per P12 layer 2; negative cases (progressive, non-interleaved,
  truncated, corrupt) fail cleanly; H.264 playback running at the same time still works (one
  core shared at job level).

#### T9 progress (work in `vpu/v4l2/jpeg/`)
- **T9a facts (board, swreg read while a decode runs):** swreg0 `0x6E645000` (ID family != 8170, so the
  vendor limits are the 8190 ones: up to 16384 wide, 268 Mpixel), swreg50 `0x1335A780`: JPEG supported
  (bit 28), progressive **not** supported (bit 22 = 0), H.264 high, bus 64 bit, max configured width 1920
  (video; the JPEG limits above are what the vendor `JpegDecInit` uses). swreg54 `0xC0040C00`, swreg99
  `0xFFFFFFFF`, swreg100 `0xFFC74780` (PP, as in T0). PP JPEG input formats (vendor `ppapi.c`):
  4:2:0 -> 1, 4:2:2 -> 4 (semi-planar), 4:0:0 -> 3, 4:4:0 -> 6, 4:4:4 -> 7 + ES 0, 4:1:1 -> 7 + ES 1
  (`swreg85[31:29]`, `swreg86[31:29]`). Registers: swreg3-8, 12-27, 40 (vendor `JpegDecInitHW`,
  `jpegdecinternal.c`); `jpeg_mode` 0/2/3/4/5/6 = 4:0:0/4:2:0/4:2:2/4:4:4/4:4:0/4:1:1; code counts in
  swreg16-26 (variable field widths); quantisation (raster order) + AC/DC values in a DMA table (swreg40,
  big-endian words, same endian/swap setup as H.264). Luma always uses HW table 1; chroma selectors
  say whether Cb/Cr use the luma table or the other one.
- **T9 design (first milestone):** own header parser (`v4l2_jpeg_parse_header` has no 4:4:0 and is a
  Kconfig dependency); OUTPUT `V4L2_PIX_FMT_JPEG` (step 16, 33..4096), visible size = size asked in
  `S_FMT` (crop, 8-pixel PP granularity) like the stateless H.264 path, header checked against the
  OUTPUT format at job time (mismatch/unsupported -> buffer error, no HW access, no hang);
  CAPTURE NV12/XRGB32/RGB565 via the PP always (combined mode, decoder output disabled, no internal
  pictures); OUTPUT queue gets a kernel mapping when the variant has the JPEG decoder; PP output limit
  4096 (>4096 JPEGs and `SOURCE_CHANGE` for stateful users come with T10). 4:1:1 only when the width
  rounds to a multiple of 32; missing DHT -> standard tables.
- Tests: `jpeg/gen_jpeg.sh` (31 images + references), `jpeg/t_jpeg_dec.c` (V4L2 tool),
  `jpeg/run_jpeg.sh` (board, `/mnt/vc8k/jpeg`), `jpeg/cmp_jpeg.sh` (host PSNR).

- **T9 board results (kernel #99):** all 26 positives decode (4:2:0/4:2:2/4:4:0/4:4:4/4:1:1/gray,
  restart markers, q10/q100, optimized Huffman, 48x48 .. 4000x3000): unscaled PSNR-Y 62-68 dB; scaled to
  1024x600 NV12 43-51 dB; XRGB 1024x600 ~43 dB. 9 negatives (progressive, arithmetic, non-interleaved,
  SOF1/16-bit DQT, truncated, corrupt, garbage, empty, 16x16) are rejected cleanly (buffer error, no hang,
  no dmesg errors). Speed: 1280x720 6 ms, 1080p 13.5 ms, 3840x2160 -> 1080p 80 ms, 12 MP 113 ms.
  Known limits: visible size has 8-pixel granularity (1000x563 is scaled from 1000x568); 4:0:0 chroma
  is 128 (ffmpeg's gray->nv12 reference has 0, not a HW difference); PP upscale limit applies to tiny
  images; SOF1 (extended sequential) unsupported, as in the vendor driver.

### T10 — JPEG userspace and camera
- Check the stock gst `v4l2jpegdec` unpatched first; patch only what fails (same approach as T4).
- `filesrc ! jpegparse ! v4l2jpegdec ! video/x-raw,width=W,height=H ! waylandsink` for stills; MJPEG
  camera (USB UVC) `v4l2src ! v4l2jpegdec ! waylandsink` at 30 fps on the panel (if a camera is available).
- Pass: scaled/letterboxed JPEG on the panel; a still next to the Qt UI keeps two planes.

#### T10 results (kernel #103, unpatched stock gst 1.22.9 `v4l2jpegdec`, `jpeg/run_gst.sh`)
- Needs Buildroot `GST1_PLUGINS_GOOD_PLUGIN_V4L2` + `_V4L2_PROBE` (the element is registered by probing
  `/dev/video0`) and `GST1_PLUGINS_BAD_PLUGIN_JPEGFORMAT` (`jpegparse`).
- Pipeline `filesrc ! jpegparse ! v4l2jpegdec ! video/x-raw,format=NV12 ! filesink`: all 23 positive
  images decode and reach EOS by themselves, PSNR-Y 62-68 dB, identical to the plain V4L2 tool
  (`cmp_jpeg.sh`); caps sizes 1920x1080, 1024x600, 1000x568 (563 rounded up to 8), 104x64; no dmesg errors.
  4k / 12 MP come out at 1920x1088 (CAPTURE default, scaled by the PP).
- Kernel fixes the stock element needed: (1) `DECODER_CMD` STOP/START with the stateful m2m helpers, no
  streaming gate (gst sends STOP while CAPTURE is still being set up; without it gst loops on empty
  buffers and never reaches EOS), LAST flag + EOS event; (2) OUTPUT `ENUM_FRAMESIZES` step 1 (with step 16
  gst built EMPTY sink caps for heights such as 1080/563/600 -> `not-negotiated` in jpegparse).
- Not working with the stock element (-> T11 gst patch): scaling or RGB via downstream caps. `v4l2videodec`
  derives its sink caps from downstream, so `v4l2jpegdec ! video/x-raw,width=1024,height=600` rejects a
  1280x720 input in `jpegparse` before any S_FMT. Still to do: patch `v4l2videodec` negotiation
  (downstream size/format -> CAPTURE S_FMT), JFIF colourspace on CAPTURE, MJPEG file (`avidemux`/`qtdemux`).

#### T11a results: gst patches for the stock `v4l2jpegdec` (scaling, RGB)
- Patch `patches/gst1-plugins-good/0001-v4l2videodec-...` (gst-plugins-good 1.22.9, `sys/v4l2/gstv4l2videodec.c`):
  (1) `sink_getcaps` answers with the probed sink caps instead of proxying downstream, because downstream
  width/height is the scaling request, not the bitstream size (the cause of `not-negotiated` in `jpegparse`);
  (2) `negotiate`: do not fixate the downstream query to the native size, fixate the chosen caps towards
  the native size, `S_FMT` the CAPTURE queue to them (the driver scales / converts in the PP) and re-acquire.
  Same idea as the H.264 `v4l2codecs` patch. Install as `package/gstreamer1/gst1-plugins-good/0001-*.patch`.
  Also in `patches/gst1-plugins-bad/`: `0005` (XRGB32->BGRx, RGB565->RGB16) and `0006` (h264dec
  add-borders), generated from the gst tree; 0004-0006 apply in order on the Buildroot-patched baseline.
- Board results, `run_gst.sh` (stock `filesrc ! jpegparse ! v4l2jpegdec ! video/x-raw,... ! filesink`):
  scaled NV12 1024x600 PSNR-Y 43-51 dB on all 1280x720/1080p/4k/12MP images; XRGB 1024x600 42-44 dB (vs
  ffmpeg bicubic), native XRGB 1080p 44.8 dB, RGB565 1080p 36 dB. 48x48 and 100x60 cannot be upscaled to
  1024x600 (PP upscale limit 3x, pipeline fails to negotiate, expected). 1000x563 stays at the 8-line crop.
- Panel (kiosk Weston, `jpeg/showj.sh`): `fdsrc ! jpegparse ! v4l2jpegdec ! video/x-raw,width=1024,height=600
  ! waylandsink` of the 1080p drone JPEG: checked by the user, correct.

### T11 — Measurements and wrap-up
- JPEG throughput (spec: 250 Mpixel/s at 200 MHz, about 225 at 180 MHz) and its effect on a
  running H.264 playback (frame delay of a 12 MP decode); CMA for JPEG, RGB 1080p and letterbox
  buffers (extend `tests/mon_cma_scaled.sh`).
- Record results here; generate the gst patch into `patches/gst1-plugins-bad/` from the gst tree
  commits (`git format-patch` from `00b38c5`), update `docs/model.md`.

## 6. Not in scope (now)
- Hardware decode for Qt still images (`QImage` / `QImageIOHandler`); stays on libjpeg-turbo (P11).
- Rotation (not available in JPEG combined mode), deinterlacing, alpha blending, standalone PP
  (memory-to-memory scaler node).
- RGB with alpha channel and RGB as a performance path (NV12 wins in memory and bandwidth).
- Upstream submission (keep patches upstream-shaped anyway).
- gst `v4l2convert` (and any standalone raw-to-raw PP node): does not fit. `v4l2convert` needs a V4L2
  mem2mem device with raw frames on both queues; our node is a stateless decoder (coded slices on OUTPUT)
  and the PP only runs in pipeline mode behind it, so the element finds nothing to use. A standalone PP
  node would be a second m2m device with its own queues and job scheduling (about T9 size) and would only
  add scaling of non-decoder sources (camera, software decode), which the GC520 2D engine (`etna2d`)
  already covers zero-copy. Revisit only if such a source needs the VPU scaler.

## 7. Risks
| # | Risk | Mitigation |
|---|---|---|
| R1 | PP scaling not synthesised (swreg100 scale level 0) | T0 first; if 0, the plan stops at T2 |
| R2 | Combined-mode scaling slower than 1080p30 | Measure in T3; fast downscale mode; lower panel targets |
| R3 | 6.18 backport touches shared code (`ref_fmt` in G2/VP9/HEVC) | Only G1 H.264 is built; S1-S6 regression after T1 |
| R4 | `v4l2jpegdec` expects stateful behaviour the Hantro m2m core lacks | Prototype the event sequence early in T9d; model on `mxc-jpeg` |
| R6 | PiP limit: guide says 1920x1088 displays, features table 4096 | Test on the board in T8; 1080p panels need only 1920x1080 |
| R7 | YCbCr to RGB matrix/range wrong or rounding differs from ffmpeg | Test with colour bars first (T7); compare PSNR, not md5 |
| R8 | JPEG register programming (tables, restart, odd sizes) is the largest single task | T9a facts first; vendor `jpegdec*` as reference; small stream set |
| R9 | A large JPEG delays H.264 frames (one core) | Measure in T11; job granularity is one image |
| R5 | CMA pressure in (a) at 1080p (about 61 MB with bitstream) | Enlarge CMA (P5); T6 |
