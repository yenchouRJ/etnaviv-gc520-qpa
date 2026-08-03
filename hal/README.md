# GC520 2D HAL — Build & Test Guide

Hardware abstraction layer for the Vivante GC520 2D GPU on Nuvoton MA35D1.
Three phases, each independently buildable and testable, culminating in a
verified zero-copy present pipeline and a Qt-independent C library.

```
hal/
├── vivante2d/     Phase 1 — raw DE op validation (no library, direct libdrm_etnaviv)
├── libetna2d/     Phase 2 — libetna2d: opaque C API wrapping the validated DE ops
└── kms/           Phase 3 — KMS scanout + benchmarks (display pipeline, no Qt)
```

---

## Hardware

| Item | Value |
|---|---|
| SoC | Nuvoton MA35D1 |
| GPU | Vivante GC520 rev 0x5341, **2D-only** (`PIPE_3D=0`) |
| `FEATURES_0` | `0xe02c7eca` (2D capable: bit 9 set) |
| DRM render node | `/dev/dri/renderD128` (etnaviv) |
| DRM display node | `/dev/dri/card0` (ma35 KMS driver) |
| Surface format | 32bpp ARGB8888 (`DE_FORMAT_A8R8G8B8`, `SWIZZLE_ARGB`) |
| Blend model | **Premultiplied** SrcOver — source must be premultiplied before submission |

---

## Phase 1 — DE Operation Validation

**Source:** `hal/vivante2d/`

Direct validation of all four GC520 Drawing Engine commands against
`libdrm_etnaviv`, without any library wrapper. Each test program
independently programs a complete DE command sequence into an
`etna_cmd_stream`, submits it, and CPU-verifies the result pixel-by-pixel.

### Source files

| File | Purpose |
|---|---|
| `etna2d_util.c/.h` | Shared DE op-builder helpers (seed of Phase 2 libetna2d) |
| `etnaviv_bitblt_test.c` | BIT_BLT opaque copy |
| `etnaviv_alpha_test.c` | Premultiplied SRC_OVER alpha blend |
| `etnaviv_stretch_test.c` | StretchBlt 2× upscale |

### Build

```sh
cd hal/vivante2d
make          # cross-compiles; binaries land in build/
make check-env  # verify toolchain + headers before building
```

Requires:
- Buildroot workspace with libdrm 2.4.120 built (for `libdrm_etnaviv` headers
  under `tests/etnaviv/`)
- `aarch64-nuvoton-linux-gnu-gcc` cross-toolchain

Override defaults if your workspace is elsewhere:
```sh
make BR=/path/to/ma35d1_br24_qt5
```

### Run on target

```sh
./build/etnaviv_bitblt_test  /dev/dri/renderD128
./build/etnaviv_alpha_test   /dev/dri/renderD128
./build/etnaviv_stretch_test /dev/dri/renderD128
# Optionally dump a BMP to verify visually:
ETNA2D_VERBOSE=1 ./build/etnaviv_bitblt_test /dev/dri/renderD128 blit.bmp
```

### Results

| Test | Operation | Result |
|---|---|---|
| `etnaviv_bitblt_test` | Opaque sub-rect copy, sentinel background check | PASS |
| `etnaviv_alpha_test` | Premult SrcOver 256×256, analytic verify ±2/channel | PASS |
| `etnaviv_stretch_test` | 2× upscale, 4-quadrant colour, interior sample verify | PASS |

**Key findings:**
- GC520 PE operates in **premultiplied alpha space**: `0x80ff0000` →
  `0x80800000` before submission. Blend factors: `DE_BLENDMODE_ONE` (src) +
  `DE_BLENDMODE_INVERSED` (dst).
- Hardware rounding: analytic result `0xff80007f`, hardware `0xff80007e`
  (1 LSB rounding in the B channel). Verified within ±2/channel.
- Command stream, FE, GPU MMU (IOMMU v2), fence, and all four DE commands
  confirmed end-to-end on GC520 rev 0x5341.

---

## Phase 2 — `libetna2d`

**Source:** `hal/libetna2d/`

The Phase 1 DE sequences repackaged behind an opaque, Qt-independent C API.
This is the reusable 2D builder that the Phase 3 benchmark programs and the
Phase 4 Qt QPA plugin link against.

### Source files

| File | Purpose |
|---|---|
| `etna2d.h` | Public API header |
| `libetna2d.c` | Implementation |
| `etna2d_selftest.c` | On-target acceptance test |

### API

```c
/* Lifecycle */
etna2d_device_open(path, &dev);
etna2d_context_create(dev, ops, &ctx);
etna2d_surface_create(dev, w, h, &surf);
etna2d_surface_from_dmabuf(dev, fd, w, h, stride, &surf);  /* PRIME import */

/* Drawing — all ops batch into ctx, not flushed until flush/finish */
etna2d_fill(ctx, dst, x, y, w, h, argb);
etna2d_blit(ctx, src, sx, sy, dst, dx, dy, w, h, blend, rop);
etna2d_stretch(ctx, src, dst, dx, dy, dw, dh);
etna2d_stretch_batch(ctx, src, dst, rects, n);  /* N dst rects, 1 submit */

/* Flush */
etna2d_flush(ctx);    /* async submit */
etna2d_finish(ctx);   /* submit + wait for fence */

/* CPU map/unmap for readback */
etna2d_surface_map(surf);
etna2d_surface_cpu_fini(surf);  /* WC drain before GPU reads CPU-written pixels */
```

### Design notes

- **Batching** — ops accumulate in `ctx`'s command stream and are submitted
  in a single `flush()`/`finish()` call. N sprites = 1 submit + 1 fence, not N.
  This is the primary throughput lever (DE fixed cost ~2.5–3 µs/op).
- **`etna2d_stretch_batch()`** — emits the heavy shared state (source, dest,
  ROP, format, PE reset) once, then only stretch-factor + clip + DRAW_2D (~16
  words, zero relocs) per rect. Auto-flushes internally every ~800 rects to
  stay under the kernel's per-submit `SZ_128K` cap.
- **`ETNA2D_STREAM_WORDS_MAX = 32,768` words (128 KB) per submit** —
  `etna2d_context_create()` clamps the stream buffer to exactly the kernel's
  hard limit (`stream_size > SZ_128K` → `EINVAL` in `etnaviv_gem_submit.c`).
  Allocating more would prevent `etna_cmd_stream_reserve()` from auto-flushing
  before the submit, causing the kernel to reject it with
  *"submit arguments out of size limits"*.  With this cap, callers passing any
  `hint_ops` value get transparent mid-frame auto-flushes instead of a hard
  error; the kernel word limit is the authoritative ceiling.
- **Surface bounds checking** — every op clips to its surface extents. An
  out-of-bounds op on the GC520 DE walks unmapped GPU-VA and hangs the GPU;
  the library prevents this unconditionally.
- **`ETNA_BO_WC` + `etna2d_surface_cpu_fini()`** — WC write buffer drain
  (`__sync_synchronize`) before any GPU read of CPU-written pixels.
- **`op_begin()` reserves `ETNA2D_OP_WORDS=128` words up front** — prevents
  libdrm's `reserve()` auto-flush from splitting a command mid-op (which would
  corrupt BO relocations).
- **Stride aligned to 64 B** per Vivante alignment spec; width rounded up to
  the 16px equivalent.
- **Silent by default** — pluggable logger (`etna2d_set_logger`) instead of
  hardwired stderr; the library is embeddable in Qt without log noise.

### Build

```sh
cd hal/libetna2d
make
# Produces:
#   build/libetna2d.a       static library
#   build/libetna2d.so.1    shared library (SONAME'd)
#   build/etna2d_selftest   on-target acceptance test
```

Override paths:
```sh
make BR=/path/to/ma35d1_br24_qt5
```

### Run selftest on target

```sh
./build/etna2d_selftest /dev/dri/renderD128
```

Covers:
- Device / surface / context lifecycle
- `etna2d_fill`, `etna2d_blit` (opaque + SrcOver), `etna2d_stretch`
- **Batched path**: 3 ops in one submit — fill + fill + premult blend,
  result verified `0xff80007f` over blue bg and `0xff807f00` over green box
  within ±2/channel
- **Batch 4 bounds robustness**: negative origin, right/bottom overhang,
  fully-off (no-op) — GPU must not hang

Expected output: all batches `[ OK ]`, no GPU hang or fence timeout.

---

## Phase 3 — KMS Scanout + Benchmarks

**Source:** `hal/kms/`

Proves the zero-copy present pipeline end-to-end without any toolkit, and
measures the DE against a scalar CPU baseline. The display allocates dumb
buffers, exports them via PRIME, etnaviv imports them into its MMU v2, the DE
renders into them, and KMS page-flips onto the panel — no CPU copy at present
time.

### Source files

| File | Purpose |
|---|---|
| `kms_display.c/.h` | Display helper: modeset, dumb buffers, PRIME export, page-flip |
| `kms_test.c` | Milestone A — display path alone, no GPU |
| `kms_gpu_test.c` | Milestone B — GPU renders into scanout BO via PRIME import |
| `bench2d.c` | Milestone C+D — compositor benchmark (fill + SrcOver) |
| `bench_stretch.c` | Milestone E — DE scaler benchmark (StretchBlt) |

### Build

```sh
cd hal/kms
make          # links libetna2d.a from ../libetna2d/build/
# Produces: build/kms_test  build/kms_gpu_test  build/bench2d  build/bench_stretch
```

`kms_test` links only `libdrm`. The other three link `libetna2d.a + libdrm`.

### Run on target

Run from a text VT with no compositor or Qt app holding `/dev/dri/card0`:

```sh
# Milestone A — display path only
./build/kms_test /dev/dri/card0 5

# Milestone B — GPU renders into scanout BO
./build/kms_gpu_test /dev/dri/card0 /dev/dri/renderD128 5

# Milestone C+D — compositor benchmark
./build/bench2d --gpu -n 64 -f 300        # GPU: fill + 64×SrcOver sprites
./build/bench2d --cpu -n 64 -f 300        # CPU baseline, same workload
./build/bench2d --gpu -n 64 --no-vsync    # render rate (no flip overhead)

# Milestone E — scaler benchmark
./build/bench_stretch --gpu --sweep       # GPU: 5 scales {0.25,0.5,1,2,4}×
./build/bench_stretch --cpu --sweep       # CPU nearest-neighbour baseline
./build/bench_stretch --gpu --scale 2     # single scale
```

### Milestone results

#### A — `kms_test`

CPU test pattern (colour bars + diagonal) appears on panel via `drmModeSetCrtc`.
Confirms modeset + scanout on `card0` with no GPU involvement. No `libetna2d`
link.

#### B — `kms_gpu_test`

GPU-rendered pixels appear on panel. Confirms the full PRIME import path:
scanout BO (CMA dumb buffer) → `drmPrimeHandleToFD` → `etna2d_surface_from_dmabuf`
→ DE fills and blends **into** the scanout BO → `drmModePageFlip`.

**Key finding:** display and GPU share only the bytes of the imported BO. There
is **no IOMMU between the engines and no cross-engine fence**. The caller must
call `etna2d_finish()` before queuing the page flip or the panel can scan out a
half-rendered frame. This ordering is enforced in every program that presents.

#### C+D — `bench2d` (compositor)

Identical workload: background fill + N × 96×96 premultiplied SrcOver sprite
composites per frame, double-buffered, animated.

| Config | ms/frame | Rate |
|---|---|---|
| CPU, 60 sprites | 121.6 ms | 7.5 FPS (vsync-capped) |
| GPU, 60 sprites |  11.0 ms | 56.9 FPS (vsync-capped) |

The GC520 DE composites the same scene ~**11× faster** than the scalar CPU loop.

**Key finding — DE bounds checking:** sprites that drifted off the surface
triggered `MMU page not present` faults. A premultiplied SrcOver blend **reads**
the destination it composites onto; that read is not bounded by the DE clip
window, so an overhanging blend reads past the BO's last mapped page and faults.
Fix: `libetna2d` clips every op to both source and destination surface extents,
making out-of-bounds op submission impossible. `etna2d_selftest` Batch 4 covers
this path.

**Key finding — `reserve()` auto-flush:** libdrm does not grow the command
buffer; when it fills it auto-flushes the partial stream, which can split a BO
relocation from its `DRAW_2D` and corrupt addressing. `op_begin()` reserves
`ETNA2D_OP_WORDS=128` words before emitting any op word, so auto-flushes only
occur at clean op boundaries.

#### E — `bench_stretch` (scaler)

One source texture (default 128×128 4-quadrant pattern) stretched to cover the
screen at five scales: `{0.25, 0.5, 1, 2, 4}×`.

| Config (src 32×32, 1× scale) | ms/frame | Mpx/s |
|---|---|---|
| CPU | 13.6 ms | — |
| GPU, per-op (before batching) | 24.4 ms | — |
| GPU, batched (`stretch_batch`) |  6.1 ms | — |

After batching, the GPU beats the CPU at every scale including the tiny-tile
worst case (8×8 tiles, ~9600 ops/frame). At src 128×128 the GPU leads across
the full sweep (e.g. 1× ≈ 82 vs 56 Mpx/s).

**Key finding — StretchBlt source addressing is in silicon.** The DE walks the
`DRAW_2D` rect and generates source texel coordinates itself:

```
srcX = SRC_ORIGIN.x + (drawX − drawRectLeft) · fx      ← done by the DE
srcY = SRC_ORIGIN.y + (drawY − drawRectTop)  · fy
```

Software only programs the scalar factor per axis, `SRC_ORIGIN`, the `DRAW_2D`
rect, and the `DE_CLIP` window. Consequences:
- `SRC_ORIGIN = (0,0)` is always correct for a placed destination tile — no
  origin fixup needed for non-zero `(dx, dy)`.
- The `DRAW_2D` rect must be the **full requested `dw×dh`** and must never be
  clipped. The DE scales the source to fill the rect; clipping the rect shrinks
  the tile. Clip only the `DE_CLIP` window.

**Key finding — DE fixed cost dominates small-tile frames.** The DE reloads its
full 2D pipeline state per op (~2.5–3 µs). Many small tiles = many ops = high
overhead regardless of pixel count. `etna2d_stretch_batch()` emits shared state
once per batch and only the per-rect delta (~16 words, zero relocs) per tile,
cutting per-tile cost several-fold.

---

## Cross-cutting hardware behaviours

These are GC520 hardware properties, not software choices. Every layer above
must respect them.

| Behaviour | Implication |
|---|---|
| **No IOMMU between display and GPU** | `etna2d_finish()` required before any page flip |
| **No cross-engine fence** | Same as above — ordering is entirely the caller's responsibility |
| **No DE bounds checking** | Out-of-bounds op → unmapped GPU-VA → `MMU page not present` → GPU hang. `libetna2d` clips every op unconditionally |
| **SrcOver destination read is not clip-bounded** | A blend op reads destination pixels beyond the write clip; `libetna2d` clips to the destination surface extent before submission |
| **`libdrm reserve()` auto-flushes** | Can split a command mid-op; use `op_begin()` to reserve the full op up front |
| **Per-submit kernel caps** | Stream ≤ 128 KB (32,768 words = `ETNA2D_STREAM_WORDS_MAX`), relocs/BOs ≤ 128 K, PMRs ≤ 128 |
| **DE pipeline state per op ~2.5–3 µs** | Batch N ops into one submit; do not flush per op |
| **StretchBlt addressing relative to DRAW_2D rect top-left** | Never clip the draw rect; clip only `DE_CLIP` |
| **Premultiplied alpha model** | Source pixels must be premultiplied; blend factors `ONE` (src) + `INVERSED` (dst) |
| **Hardware rounding** | Blend results may differ ±1 LSB per channel from the analytic value |

---

## Build summary

```sh
# Phase 1
cd hal/vivante2d && make

# Phase 2
cd hal/libetna2d && make
# → build/libetna2d.a  build/libetna2d.so.1  build/etna2d_selftest

# Phase 3 (needs Phase 2 built first)
cd hal/kms && make
# → build/kms_test  build/kms_gpu_test  build/bench2d  build/bench_stretch
```

All three cross-compile for `aarch64-nuvoton-linux-gnu` cleanly with zero
warnings. Artifacts land in each phase's `build/` subdirectory.

Override the Buildroot workspace root for all phases:
```sh
make BR=/path/to/ma35d1_br24_qt5
```

---

## Deploy to target

```sh
BOARD=root@192.168.1.100
HAL=/home/joeylu/opencode/graphic/drm-2d-gc520/hal

# libetna2d shared library (needed by Phase 3 programs and the QPA plugin)
scp $HAL/libetna2d/build/libetna2d.so.1 $BOARD:/usr/lib/
ssh $BOARD "ln -sf libetna2d.so.1 /usr/lib/libetna2d.so"

# Phase 1 tests
scp $HAL/vivante2d/build/etnaviv_{bitblt,alpha,stretch}_test $BOARD:/tmp/

# Phase 2 selftest
scp $HAL/libetna2d/build/etna2d_selftest $BOARD:/tmp/

# Phase 3 programs
scp $HAL/kms/build/{kms_test,kms_gpu_test,bench2d,bench_stretch} $BOARD:/tmp/
```
