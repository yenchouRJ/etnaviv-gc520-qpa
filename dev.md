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

**Status:** built clean and verified on hardware — `etna2d_selftest` passes
all batches (including the stretch-placement and batched-stretch regressions
and the Batch 4 out-of-bounds robustness cases).

---

## Phase 3 — KMS scanout + benchmarks (display half, no Qt)

**Source:** `tests/kms/`
**Display:** DRM/KMS on `/dev/dri/card0` (ma35 driver), legacy modeset path.
**Render:** GC520 via `libetna2d` on `/dev/dri/renderD128`.

Goal: prove the zero-copy present pipeline end-to-end, separately from any
toolkit, and measure the DE against a scalar CPU baseline. The display
allocates a contiguous dumb buffer, exports it via PRIME, etnaviv imports it
into its MMU v2, the DE renders into it, and KMS page-flips it onto the panel —
no CPU copy at present time.

Four programs build on each other in pipeline order; each isolates one layer
so a failure points at exactly one thing:

| Program | Layer proven |
|---|---|
| `kms_test`       | Display path alone — modeset + scanout, no GPU |
| `kms_gpu_test`   | + the PRIME import path — GPU renders **into** the scanout BO |
| `bench2d`        | + double-buffered animation, compositor benchmark (fill + SrcOver) |
| `bench_stretch`  | + the DE scaler (StretchBlt), scaling benchmark |

### Display helper (`kms_display.[ch]`)

Encapsulates the display side so the milestone programs stay small:
`kms_open` (pick first connected connector + usable crtc + preferred mode),
`kms_fb_create` (dumb buffer → `drmModeAddFB2`, `DRM_FORMAT_XRGB8888`),
`kms_fb_export_dmabuf` (`drmPrimeHandleToFD`), `kms_set_crtc`,
`kms_page_flip` + `kms_wait_flip` (vblank event via `drmHandleEvent`).
Saves/restores the original CRTC on close. Legacy modeset (not atomic) —
sufficient for fullscreen scanout and simpler.

ma35 capabilities confirmed from the driver source (`ma35_plane.c`):
`DRIVER_GEM|MODESET|ATOMIC`, GEM-DMA dumb buffers with VMAP, PRIME, and
`ARGB8888`/`XRGB8888` primary-plane formats.

Run from a text VT with no compositor/Qt holding the display:
```
./kms_test        /dev/dri/card0 5
./kms_gpu_test    /dev/dri/card0 /dev/dri/renderD128 5
./bench2d --gpu -n 64 -f 300      ;  ./bench2d --cpu -n 64 -f 300
./bench_stretch --gpu --sweep     ;  ./bench_stretch --cpu --sweep
```

All four cross-compile cleanly for aarch64 (no warnings). `kms_test` links
only `libdrm`; the other three link `libetna2d.a` + `libdrm`.

---

### 1. `kms_test` — display path stands alone

**Purpose.** Prove on-panel output with zero GPU involvement, so any later
display problem is not a modeset bug. Allocates a dumb buffer, `AddFB2`, writes
a CPU test pattern (colour bars + diagonal), `SetCrtc`, holds it on screen.
Does not even link `libetna2d`.

**Result.** Test pattern displays correctly; modeset + scanout on `card0` is
sound. This is Milestone A of the pipeline.

---

### 2. `kms_gpu_test` — the PRIME import path

**Purpose.** Prove the zero-copy render target: scanout BO → PRIME export →
`etna2d_surface_from_dmabuf` → the DE fills + premult-blends **into** the
scanout BO → present. Every on-screen pixel is GPU-produced and reaches the
panel with no CPU copy, exercising etnaviv's MMU v2 import of a foreign
(display-allocated, CMA) buffer.

**Result.** GPU-rendered pixels appear on the panel. Confirms the two engines
can share a buffer by bytes alone: the DE writes through its own MMU mapping of
the imported dma-buf, `card0` scans out the same physical pages. Milestone B.

**HW behaviour exposed — no IOMMU coupling.** Display and GPU share only the
bytes of the imported BO. There is no IOMMU between the engines and no
cross-engine fence: the caller must **explicitly fence** GPU work
(`etna2d_finish`) before queuing the page flip, or the panel can scan out a
half-rendered frame. This ordering is the caller's responsibility and is
enforced in every program that presents.

---

### 3. `bench2d` — compositor benchmark (fill + SrcOver)

**Purpose.** Measure the DE as a **compositor** against a scalar CPU baseline
on an identical workload: a background fill plus N × 96×96 premultiplied
source-over sprite composites per frame, double-buffered (two scanout FBs,
ping-pong on vblank), animated. Only the pixel-touching engine differs.
Knobs: `-n` sprites, `-f` frames, `--no-vsync`, `--card`, `--render`.

**Result (1024×600@60, 96×96 sprites):**

| Run | render ms/frame | rate |
|---|---|---|
| CPU, 60 sprites | 121.6 | 7.5 FPS (vsync-capped) |
| GPU, 60 sprites | 11.0  | 56.9 FPS (vsync-capped) |

The GC520 DE composites the same scene ~**11× faster** than the scalar CPU
loop. Milestones C+D.

**HW behaviour exposed — the DE has no implicit bounds checking (MMU fault /
GPU hang).** Once sprites bounced around the screen the GPU hung intermittently
with `MMU 0 fault (page not present)`. A devcoredump captured on hang pinned it
exactly:

```
scanout BO iova  : 0x2e3000
scanout BO size  : 0x258000   (2,457,600 = 600 rows × 4096 B stride)
scanout BO end   : 0x2e3000 + 0x258000 = 0x53b000
fault address    : 0x53b5c0   →  0x5c0 past the end  (row 600 of a 600-row buffer)
```

Every fault address (`0x53b5c0`, `0x53b700`, `0x53c2c0`) sat 1–2 rows past the
buffer end. Root cause was an **application bug**: `sprites_step` reversed
velocity at the edge without **clamping** position, so a fast sprite overshot
to `dy + SPRITE_H = 601 > 600`. A `SRC_OVER` blend **reads** the destination it
composites onto, so the DE read destination row 600 — past the BO's last mapped
page — and MMU-faulted. (A plain fill only *writes*, and the write is bounded
by the DE clip window, which is why fills never faulted; only the blend's
destination *read* runs unchecked.) With no IOMMU and no DE bounds check, an
out-of-bounds op simply walks unmapped GPU-VA and faults.

*Fixes.* Two, the second being the important one:
- **App (`bench2d.c`):** `sprites_step` now clamps position to
  `[0, dim − SPRITE]` in addition to reflecting velocity.
- **Library (`libetna2d.c`):** every op now clips to the surface so no caller
  input can hang the GPU — `etna2d_fill` clips to the destination;
  `etna2d_blit`/`etna2d_blend_over` clip to **both** source and destination
  extents (the blend's destination read is not bounded by the DE clip window)
  and drop fully-off-surface ops. A 2D library must never let a bad rectangle
  hang the GPU. `etna2d_selftest` gained **Batch 4** covering negative origin,
  right/bottom overhang, and fully-off (no-op) cases.

**HW behaviour exposed — `etna_cmd_stream_reserve()` auto-flushes mid-op.**
libdrm does **not** grow the command buffer; when it fills it *auto-flushes*
(submits) the partial stream. In a batched context that flush could land
**inside** an op, splitting its BO relocation from the `DRAW_2D` that consumes
it → the DE draws from a stale address → MMU fault. `op_begin()` reserves a
full worst-case op (`ETNA2D_OP_WORDS = 128`) up front so any auto-flush occurs
at a clean op boundary, and resets `pending` if a flush fired so
`flush`/`finish` don't double-submit. Kernel per-submit caps also observed:
`stream_size > SZ_128K`, `nr_relocs`/`nr_bos > SZ_128K`, `nr_pmrs > 128`.

**Measurement corrections.** (a) `--no-vsync` originally queued page flips
without waiting the prior vblank → `EBUSY` → dropped frames; it now skips
modeset/present entirely and reports a **render rate** from GPU-fenced render
time (real DE fill-rate). The vsync path additionally reports the real
vsync-capped **present rate**. (b) The CPU baseline was fixed to blend the same
`0x80800000` premultiplied red as the GPU path so both composite the identical
scene.

---

### 4. `bench_stretch` — DE scaler benchmark (StretchBlt)

**Purpose.** Measure the DE **scaler** (point-sampled StretchBlt) against a CPU
nearest-neighbour baseline. One fixed source texture (default 128×128,
4-quadrant colour pattern) is stretched to `src·scale` and **tiled to cover the
screen**, double-buffered. Scaler cost tracks the **destination** area (each
output pixel samples the source), so it reports **destination Mpixels/sec** and
**sweeps** `{0.25, 0.5, 1, 2, 4}×` because up- and down-scaling stress the
sampler and memory system differently. Both engines use the same
`((size−1)<<16)/(dst−1)` mapping, so they produce identical pixels.
Options: `--scale F`, `--sweep`, `--src WxH`, `-f FRAMES`, `--no-vsync`,
`--card`, `--render`.

**HW behaviour exposed — how the DE addresses the source.** The first run
showed lower tiles vertically **compressed** on the GPU while the CPU reference
was correct. Per-row pixel dumps on hardware pinned down two independent
mechanisms (an earlier "absolute-dest-coordinate / negative source-origin"
theory was wrong and was discarded).

First, the key point about *where the coordinate math happens*: **it is all in
silicon, not in our code.** For a stretch the software programs only scalar
registers — the `.16` step factors `fx`/`fy`, `SRC_ORIGIN`, the `DRAW_2D`
rectangle, and the `DE_CLIP` window. We never compute a source coordinate for
any destination pixel. The DE walks the `DRAW_2D` rect and, per output pixel,
generates the source texel itself, effectively:

```
srcX = SRC_ORIGIN.x + (drawX − drawRectLeft) · fx     ← all done by the DE
srcY = SRC_ORIGIN.y + (drawY − drawRectTop)  · fy
```

So "samples relative to the draw-rect top-left" is a **hardware feature** — the
DE subtracts the `DRAW_2D` rect origin (`drawRectLeft/Top`) before scaling. It
is *not* a software formula we apply; the only software math is the single
scalar factor per axis. The two consequences:

1. **Sampling is relative to the DRAW_2D rect's top-left**, not absolute dest
   coordinates. `SRC_ORIGIN`/`SRC_SIZE` are the source sampling **window** (in
   source texels); because the HW already subtracts the rect origin, a placed
   dest rect at `(dx,dy)` needs **no** origin correction — `SRC_ORIGIN = (0,0)`
   is correct everywhere. *Proof (both directions):* with `SRC_ORIGIN = 0` a
   tile at dest `(128,128)` rendered correctly — if the HW used absolute
   coordinates it would have sampled `128·fx`, off the source, and produced
   garbage; and *forcing* `SRC_ORIGIN = (dx,dy)` to "correct" for placement
   turned the tile all-black, because `SRC_ORIGIN` is a source-texel base and
   `(128,128)` is outside a 128 px source. `etna2d_stretch` therefore just uses
   `set_source(…,0,0)`.

2. **Scale geometry is derived from the DRAW_2D rect size** (the source spans
   `SRC_SIZE` texels across `drawRectWidth/Height` destination pixels), while
   DE_CLIP bounds the actual writes. This caused the compression: the original
   code **clipped the draw rect** for a partially-off tile, so the HW re-spread
   the same source across the smaller (clipped) rect. *Fix:* keep the `DRAW_2D`
   rect the FULL requested `dw × dh` and clip **only** the DE_CLIP window. The
   draw rect may legitimately extend past the surface — the DE walks source
   pixels but only *writes* inside the clip window, so no unmapped dest page is
   touched (consistent with the `bench2d` "no bounds check" finding). The
   selftest stretches into an offset rect and an edge-overhanging rect and
   checks the quadrant pattern is not compressed.

**HW behaviour exposed — per-op state reload dominates tiny-tile frames.** With
placement correct, the tiny-tile case was still far slower than the CPU. The DE
**reloads its full 2D pipeline state per op**; measured fixed cost ~2.5–3 µs/op.
Scaler *pixel* throughput only dominates once tiles are large (≥ ~1× here);
below that, frame time is `ops × per-op-cost`. A heavy downscale (8×8 tiles →
~9600 ops/frame) was ~4× slower than the CPU's flat pixel loop — not a bug, a
property of the engine (no draw instancing). Two changes removed the overhead:

- **Moved the PE 2D cache flush out of the per-op tail** into
  `etna2d_flush()`/`etna2d_finish()` — emitted **once per submit** instead of
  after every op (each per-op flush is a full pipeline stall). Safe because the
  DE processes DRAW_2D in order and the flush is only needed before the buffer
  is consumed by scanout or CPU readback.
- **Added `etna2d_stretch_batch()`** — stretch one source into an array of dest
  rects, emitting the heavy shared state (source, dest, ROP, format,
  `pe_reset`) **once**, then only stretch-factor + clip + `DRAW_2D` per rect
  (~16 words, **zero relocs** per rect). It auto-flushes internally every ~800
  rects to stay under the kernel's per-submit `SZ_128K` cap, re-emitting shared
  state after each flush. This is also the natural shape for a Qt QPA plugin,
  which flushes many raster ops per frame — so it earns its place regardless of
  the synthetic benchmark.

**Result (1024×600, src 32×32, `--no-vsync`):** the 0.25× case (8×8 tiles,
~9600 ops/frame) went **24.4 ms → 6.1 ms** (~4×) after batching; the GPU now
beats the CPU at every scale, including the tiny-tile worst case (6.1 vs
13.6 ms). At the default src 128×128, the GPU leads the CPU across the sweep
(e.g. 1× ≈ 82 vs 56 Mpx/s). Practical caveat: 9600 tiny tiles is a synthetic
worst case, rare in a real Qt app — the durable win is one-submit batching with
shared state, not the headline ratio.

---

### Cross-cutting HW behaviours proven in Phase 3

- **No IOMMU between display and GPU, no cross-engine fence.** They share only
  bytes; the caller must explicitly fence GPU work before the flip.
- **The DE has no implicit bounds checking.** Any op running off a surface
  walks unmapped GPU-VA → `MMU page not present` → hang. Blends fault on the
  destination *read* even when the write is clipped; the library now clips
  every op. Bounds are wholly the caller/library's responsibility.
- **libdrm `reserve()` auto-flushes mid-op**; ops are made atomic via
  `op_begin()`. Kernel per-submit caps (`SZ_128K` stream/relocs/bos, 128 pmrs)
  bound batch size — the stretch batch flushes internally to respect them.
- **StretchBlt source addressing is done in silicon**, relative to the DRAW_2D
  rect top-left (`srcX = SRC_ORIGIN + (drawX − drawRectLeft)·fx`), and scaled by
  the draw-rect size. Software only sets the scalar factor, `SRC_ORIGIN`, the
  rect and the clip window — so placement needs no origin fixup, and the draw
  rect must never be clipped (clip DE_CLIP only) or the tile is rescaled.
- **The DE reloads full pipeline state per op (~2.5–3 µs)** — throughput on
  many-small-op frames is won by batching shared state and flushing once per
  submit, not by pixel rate.

**Status:** all four programs and `libetna2d` build clean (aarch64, no
warnings); the display, import, compositor and scaler paths are all verified on
hardware, including the placement and batching fixes.
