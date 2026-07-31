# etnaviv2d QPA — Test Suite

Documents each test: what it exercises, how to run it, and what a passing result proves.

---

## test_blitter

**Step 1 — EtnaBlittable unit test (no display)**

Tests `EtnaBlittable` and `EtnaBlittablePlatformPixmap` in isolation without a
KMS display.  Creates GPU surfaces, exercises every blitter virtual, and reads
back via CPU to compare against a reference.

**Proves:** HW fill/blit/blend ops execute correctly; premult format is
preserved; raster fallback does not crash when the source is a
`QRasterPlatformPixmap`.

---

## test_screen

**Step 2 — KMS modeset + scanout (no Qt painting)**

Opens `/dev/dri/card0`, performs modeset, allocates two dumb BOs, imports them
into etnaviv, fills with `etna2d_fill`, and page-flips.

**Proves:** Both scanout surfaces are importable via PRIME; flip + vblank wait
works; `QCoreApplication` (not `QGuiApplication`) avoids a double-modeset
`EACCES`.

---

## test_integration

**Step 3 — First Qt window on panel**

Runs a minimal `QWidget` with a `paintEvent` that calls `fillRect`, under
`QT_QPA_PLATFORM=etnaviv2d`.

**Proves:** Full plugin stack initialises; widget appears on panel; resize works;
no GPU faults.

---

## test_benchmark

**Step 4 — GPU vs CPU frame-rate benchmark**

Animates N sprites per frame using `QPainter::drawPixmap` (alpha or opaque
mode).  Run once with `etnaviv2d` and once with `linuxfb`; compare FPS.

Both platforms are vsync-paced: `etnaviv2d` via `drmModePageFlip` vblank wait;
`linuxfb` via `FBIO_WAITFORVSYNC` ioctl.

```
QT_QPA_PLATFORM=etnaviv2d  ./test_benchmark 120 128         # alpha
QT_QPA_PLATFORM=linuxfb    ./test_benchmark 120 128
QT_ETNAVIV2D_BENCH_OPAQUE=1 QT_QPA_PLATFORM=etnaviv2d  ./test_benchmark 120 128
QT_ETNAVIV2D_BENCH_OPAQUE=1 QT_QPA_PLATFORM=linuxfb    ./test_benchmark 120 128
```

**Proves:** etnaviv2d wins alpha-blend workload (SRC_OVER via GC520 PE);
linuxfb is competitive on raw opaque copy at low sprite counts (DE fixed cost
~2.5–3 µs/op outweighs CPU memcpy only for small tiles); vsync pacing makes FPS
numbers directly comparable.

---

## test_slideshow

**Risk #2 — Raster-source pixmap path on hardware**

### What it tests

`QImage` (CPU buffer) and `QPixmap` (EtnaBlittable GPU surface) are fundamentally
different objects.  When Qt draws a `QPixmap` onto another `QPixmap` it can go
through the GPU blitter.  When it draws a `QImage` it must fall back to the CPU
raster engine writing directly into the WC-mapped backing buffer.

This test verifies that the CPU raster fallback produces correct pixels — no
corruption, no colour shift, no cache-ordering artefacts — by showing both paths
**side by side on screen** so any difference is immediately visible.

### Layout

The screen is split into two equal halves.  Both halves show the same image
advancing in sync at `interval_ms` per frame.

```
┌──────────────────────┬──────────────────────┐
│  QPixmap / blitter   │  QImage / raster     │
│                      │                      │
│      [image]         │      [image]         │
│                      │                      │
│  frame N  parrot-N.png  interval=800ms      │
└──────────────────────┴──────────────────────┘
     left half                right half
```

### How each half works

```
LEFT (QPixmap / blitter)          RIGHT (QImage / raster)
────────────────────────          ───────────────────────
disk                              disk
 │ QImage::load()                  │ QImage::load()
 ▼                                 ▼
QImage (CPU buf)                  QImage (CPU buf)
 │ QPixmap::fromImage()            │  (kept as-is)
 │   doLock() map BO               │
 │   CPU copy → BO                 │
 │   doUnlock() unmap BO           │
 ▼                                 ▼
EtnaBlittable BO                  (raw QImage, no GPU upload)
 │                                 │
 │ p.drawPixmap(leftDst, pixmap)   │ p.drawImage(rightDst, image)
 │                                 │
 ▼                                 ▼
etna2d_blit(src_BO → backing_BO)  doLock() → CPU reads QImage
 GPU DMA, no CPU involved            writes into WC-mapped backing_BO
                                   doUnlock() (cache flush + fence)
 │                                 │
 └──────────┬──────────────────────┘
            ▼
     flush() — same for both halves
     etna2d_blit(backing_BO → scanout_BO)
     drmModePageFlip → display
```

### Run

```sh
QT_QPA_PLATFORM=etnaviv2d ./test_slideshow ../../images 800 3
QT_QPA_PLATFORM=linuxfb   ./test_slideshow ../../images 800 3
```

Arguments: `[image_dir [interval_ms [loops]]]`  
Defaults: `../../images`, `800`, `2` (0 = infinite).

### Pass criteria

1. Left and right halves look **identical** — same colours, no corruption.
2. Images are displayed with **aspect ratio preserved** (black bars, not stretched).
3. `dmesg` shows no GPU faults or kernel warnings.
4. Process exits cleanly after `loops` cycles; terminal is restored.
5. Ctrl-C at any point exits cleanly (SIGINT → `QCoreApplication::quit()`).

### What it proves

- The CPU raster fallback (`doLock` → CPU write into WC BO → `doUnlock`) writes
  correct pixels.  The `etna2d_surface_cpu_fini` + `etna2d_finish` barrier in
  `doUnlock` is sufficient to make CPU writes visible to the GPU before the
  scanout blit.
- `Format_ARGB32_Premultiplied` is consistent between the raster engine, the
  GPU surface, and the scanout blit — no format mismatch artefacts.
- **Risk #2 closed.**
