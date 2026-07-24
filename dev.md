# GC520 2D Acceleration — Development Log

---

## Phase 1 — DE Operation Validation

**Hardware:** Nuvoton MA35D1, Vivante GC520 rev 0x5341  
**GPU FEATURES_0:** `0xe02c7eca`  
**Test node:** `/dev/dri/renderD128`  
**Source:** `tests/vivante2d/`

### Tests

All three tests pass. Source: `tests/vivante2d/`.

#### BIT_BLT (`etnaviv_bitblt_test`)

Copies a 128×96 sub-window from a position-dependent ARGB gradient source
(origin `32,48`) into an offset in the destination (origin `16,8`).
Destination was pre-cleared to sentinel `0xff102030` by a GPU CLEAR op.
Every pixel verified: blitted region matches source, background intact.

```
[ OK ] BIT_BLT verified: 128x96 window copied, background intact
```

#### Alpha blend (`etnaviv_alpha_test`)

**GC520 PE operates in premultiplied-alpha space.**
Source must be premultiplied before submission (`0x80ff0000` non-premult
→ `0x80800000` premult). Blend factors: src = `DE_BLENDMODE_ONE`,
dst = `DE_BLENDMODE_INVERSED`, implementing premultiplied SrcOver:

```
out.rgb = src.rgb + dst.rgb × (1 − src.a) / 255
out.a   = src.a  + dst.a   × (1 − src.a) / 255
```

Analytic result `0xff80007f`, hardware produced `0xff80007e` (1 LSB
rounding in B channel). Verified across all 65536 pixels within ±2/channel.

```
[INFO] analytic blend = 0xff80007f, hw sample = 0xff80007e (tol +/-2)
[ OK ] alpha blend verified across 256x256 within +/-2/channel
```

**Phase 4 note:** Qt's `QRasterPaintEngine` produces
`QImage::Format_ARGB32_Premultiplied` internally, matching the hardware
expectation exactly — no pixel conversion needed in the QPA plugin.

#### STRETCH_BLT (`etnaviv_stretch_test`)

2× upscale of a 128×128 4-quadrant source (R/G/B/W) to 256×256.
Stretch factors: `x=0x07f7f y=0x07f7f` (`.16` fixed-point,
formula `(src−1 << 16) / (dst−1)`). Interior samples at 1/4 and 3/4 points
of each destination quadrant all correct.

```
[ OK ] STRETCH_BLT verified: 4 quadrants scaled and placed 2x
```

### What is now proven

| DE operation | Register path | Status |
|---|---|---|
| Solid fill | `DEST_CONFIG_COMMAND_CLEAR` + `CLEAR_PIXEL_VALUE32` | ✅ (from Phase 0) |
| Opaque copy | `DEST_CONFIG_COMMAND_BIT_BLT`, `SRC_ADDRESS/STRIDE/CONFIG`, ROP4 `0xCC` | ✅ |
| Alpha blend | BIT_BLT + `ALPHA_CONTROL_ENABLE_ON`, `ALPHA_MODES` (premult SrcOver) | ✅ |
| Scale | `DEST_CONFIG_COMMAND_STRETCH_BLT`, `STRETCH_FACTOR_LOW/HIGH` | ✅ |

Command stream, FE, GPU MMU (IOMMU v2), fence, and all four DE commands
are confirmed working end-to-end on GC520 rev 0x5341.

### Shared helper layer (`etna2d_util`)

`etna2d_util.c/.h` packages the four validated op-builders as the seed of
the Phase 2 `libetna2d` module. Each function emits a complete, stateless
DE command sequence (full state setup + `DRAW_2D` rectangle) into an
`etna_cmd_stream`. No state leaks between ops.

---

## Phase 2 — `libetna2d` (thin userspace HAL)

**Source:** `tests/libetna2d/`  
**Artifacts:** `libetna2d.so.1` (SONAME'd shared), `libetna2d.a` (static),
`etna2d.h` (public API), `etna2d_selftest` (on-target acceptance test).

The validated Phase 1 DE sequences were repackaged behind an opaque,
Qt-independent, C API. This is the reusable 2D builder that both the Phase 3
benchmark app and the Phase 4 Qt QPA plugin link against.

### API shape (`etna2d.h`)

| Type | Role |
|---|---|
| `etna2d_device`  | DRM render node + 2D-capable core (FEATURES_0 bit 9) + `ETNA_PIPE_2D` |
| `etna2d_surface` | 32bpp ARGB drawable — created fresh **or** imported from a PRIME dma-buf fd |
| `etna2d_context` | accumulating `etna_cmd_stream`; ops batch until `flush`/`finish` |

Operations: `etna2d_fill`, `etna2d_blit` (sub-rects, selectable ROP, opaque
or premult SrcOver), `etna2d_blend_over`, `etna2d_stretch`.

### Design decisions

- **Surfaces bundle `{bo, stride, w, h}`** — callers pass one handle, not
  four loose params. Maps cleanly onto a Qt pixmap / KMS scanout buffer.
- **`etna2d_surface_from_dmabuf`** wraps `etna_bo_from_dmabuf`, providing the
  import half of the Phase 3 KMS flow (dumb buffer on card0 → PRIME export →
  import into etnaviv MMU v2) up front.
- **Batching** — a context holds one persistent command stream; N ops
  accumulate into a single submit. This is the real throughput lever
  (N sprites = 1 submit + 1 fence, not N). `flush()` is async, `finish()`
  blocks on the fence for CPU-coherent readback.
- **Stride auto-aligned** to 64B (`ETNA2D_ALIGN`) per the Vivante alignment
  doc; width rounded up to 16px equivalent.
- **`ETNA_BO_WC`** mappings + a `__sync_synchronize()` drain in
  `etna2d_surface_cpu_fini()` before the GPU reads CPU-written pixels.
- **Pluggable logger** (`etna2d_set_logger`) instead of hardwired stderr —
  silent by default, so the library is embeddable in Qt.

### Acceptance test (`etna2d_selftest`)

Exercises the full API — device/surface/context lifecycle, all four ops,
and specifically the **batching path** (3 ops in one submit) that Phase 1
did not cover. CPU-verifies:
- batched fill + fill + premult blend (result `0xff80007f` over blue bg,
  `0xff807f00` over a green box) within ±2/channel;
- opaque sub-rect blit (copied region, background intact, outside untouched);
- 2× stretch (four quadrant colours land in the right destination cells).

Cross-compiles cleanly for aarch64 (no warnings). `libetna2d.so.1` exports
only the `etna2d_*` API; its sole external dependency is `libdrm_etnaviv`.

**Status:** built and staged. On-hardware run of `etna2d_selftest` pending.

---

## Phase 3 — KMS scanout milestone (display half, no Qt)

**Source:** `tests/kms/`  
**Display:** DRM/KMS on `/dev/dri/card0` (ma35 driver), legacy modeset path.
**Render:** GC520 via libetna2d on `/dev/dri/renderD128`.

Proves the zero-copy present pipeline separately from any toolkit: the
display allocates a contiguous dumb buffer, exports it via PRIME, etnaviv
imports it into its MMU v2, the DE renders into it, and KMS page-flips it
onto the panel — no CPU copy at present time.

### Display helper (`kms_display.[ch]`)

Encapsulates the display side so the milestone programs stay small:
`kms_open` (pick first connected connector + usable crtc + preferred mode),
`kms_fb_create` (dumb buffer → `drmModeAddFB2`, `DRM_FORMAT_XRGB8888`),
`kms_fb_export_dmabuf` (`drmPrimeHandleToFD`), `kms_set_crtc`,
`kms_page_flip` + `kms_wait_flip` (vblank event via `drmHandleEvent`).
Saves/restores the original CRTC on close. Legacy modeset (not atomic) —
sufficient for fullscreen scanout and simpler.

ma35 capabilities confirmed from the driver source: `DRIVER_GEM|MODESET|
ATOMIC`, GEM-DMA dumb buffers with VMAP, PRIME, and `ARGB8888`/`XRGB8888`
primary-plane formats (`ma35_plane.c`).

### Milestones

| Program | Proves |
|---|---|
| `kms_test` | Milestone A — on-panel output: dumb buffer → `AddFB2` → CPU test pattern (colour bars + diagonal) → `SetCrtc`. Display path stands alone (no GPU, does not even link libetna2d). |
| `kms_gpu_test` | Milestone B — the import path: scanout → PRIME export → `etna2d_surface_from_dmabuf` → DE fills + premult blend render **into** the scanout BO → present. Every on-screen pixel is GPU-produced through etnaviv's MMU. |
| `bench2d` | Milestone C+D — double-buffered (two scanout FBs, ping-pong on vblank) animated benchmark; `--gpu` (libetna2d fill + SrcOver) vs `--cpu` (software premult SrcOver) over the identical scene. Reports FPS and ms/frame. |

### `bench2d` methodology

Identical workload both modes: background fill + N × 96×96 premultiplied
source-over sprite composites per frame, double-buffered, animated. Only the
pixel-touching engine differs (GC520 DE vs a scalar CPU compositor). Knobs:
`-n` sprites, `-f` frames, `--no-vsync` (measure uncapped render throughput
vs vsync-capped present rate), `--card`, `--render`. The `--no-vsync`
render-time figure is the clean fill-rate comparison; the vsync FPS is the
real present rate.

Cross-compiles cleanly for aarch64 (no warnings); `bench2d`/`kms_gpu_test`
link `libetna2d.a` + `libdrm`, `kms_test` links only `libdrm`.

**Status:** built and staged. On-hardware runs pending. Run from a text VT
with no compositor/Qt holding the display:
```
./kms_test /dev/dri/card0 5
./kms_gpu_test /dev/dri/card0 /dev/dri/renderD128 5
./bench2d --gpu -n 64 -f 300   ;   ./bench2d --cpu -n 64 -f 300
./bench2d --gpu -n 64 -f 300 --no-vsync   # uncapped fill-rate
```
