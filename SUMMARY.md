# GC520 Qt5 2D Acceleration — Project Summary

Target: Nuvoton MA35D1 SoC, Vivante GC520 (2D-only GPU), Linux 6.6, Qt5.

Buildroot:
- /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
drivers:
- {buildroot}/output/build/linux-custom/drivers/gpu/drm
- /home/joeylu/opencode/graphic/drm-2d-gc520/drivers/gpu/drm
devicetree:
- {buildroot}/output/build/linux-custom/arch/arm64/boot/dts/nuvoton/ma35d1.dtsi
- {buildroot}/output/build/linux-custom/arch/arm64/boot/dts/nuvoton/ma35d1-som-256m.dts
packages:
- {buildroot}/output/build/libdrm-2.4.120
- {buildroot}/output/build/qt5base-
vivante's vender support with tests:
- /home/joeylu/opencode/graphic/drm-2d-gc520/drivers/ref/GC520L

---

## Hardware

| Property | Value |
|---|---|
| GPU | Vivante GC520, 2D-only (`PIPE_2D=1`, `PIPE_3D=0`) |
| MMIO | `0x40280000`, IRQ SPI 27 |
| Clock | 250 MHz (`GFX_GATE`) |
| Reset | `MA35D1_RESET_GFX` |
| FE | Wait-Link FE (WLFE) |
| MMU | Internal IOMMU v2 (two-level, MTLB+STLB) |
| Security | `ETNA_SEC_NONE` |
| 2D features | BitBlit, StretchBlit, FilterBlit, AlphaBlend, FastScaler, PE 2.0 |

---

## What Was Done

### 1. Kernel driver bring-up (etnaviv, Linux 6.6)

Three patches applied to `drivers/gpu/drm/etnaviv/`:

**`etnaviv_gpu.c` — reset deassert**
The stock driver never releases `MA35D1_RESET_GFX`. All MMIO reads returned
`0x0` → `model: GC0`. Fix: add `devm_reset_control_get_optional()` +
assert/deassert in `etnaviv_gpu_init()` before `etnaviv_hw_identify()`.

### 2. DTS node

```dts
gpu@40280000 {
    compatible = "vivante,gc";
    reg = <0x0 0x40280000 0x0 0x10000>;
    interrupts = <GIC_SPI 27 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&clk GFX_GATE>, <&clk GFX_GATE>;
    clock-names = "core", "shader";
    resets = <&reset MA35D1_RESET_GFX>;
    reset-names = "gpu";
    status = "okay";
};
```

Do not add `dma-coherent` — it skips `arch_sync_dma_for_device` in
`dma_map_sgtable`, causing the GPU to read stale command buffers from DDR.

### 3. Test app fix (libdrm-2.4.120)

`etnaviv_2d_test.c:246` — changed `ETNA_BO_UNCACHED` → `ETNA_BO_WC`.

Root cause: `ETNA_BO_UNCACHED` maps pages with `pgprot_noncached` →
`MT_DEVICE_nGnRnE` on arm64 (Device memory). `memset` on Device memory
causes a synchronous external abort → `SIGBUS`. `ETNA_BO_WC` uses
`pgprot_writecombine` → `MT_NORMAL_NC`, which is ordinary memory.

### 4. Tests passing

```
etnaviv_2d_test /dev/dri/renderD128   → Compare OK
etnaviv_bo_cache_test ...             → ok
etnaviv_cmd_stream_test ...           → ok
```

`intr 0x00000001` confirms GPU fence completion. The DE CLEAR command wrote
`0xff40ff40` across a 256×256 BO in a 16×16 grid of 4×4-pixel tiles.

What this proves: kernel driver, FE, GPU MMU (IOMMU v2), GEM BO lifecycle,
DRM scheduler, and fence signalling all work end-to-end on GC520.

What this does NOT prove: BitBlit, alpha blend, stretch — those DE commands
were not exercised. (The test only uses CLEAR + DRAW_2D.)

### 5. Qt5 display baseline

`QT_QPA_PLATFORM=linuxfb` works. Qt5 widgets render on-screen via `/dev/fb0`
using the CPU raster engine. This is the baseline to beat with GPU acceleration.

---

## Stack Clarification

```
Qt5 app (QPainter API — unchanged)
    │
    ▼
Custom Qt5 QPA plugin          ← new work (backing store + paint engine)
    │
    ▼
libetna2d  (thin 2D op builder) ← new work — the "userspace HAL" to write
    │  fill / blit / blend / stretch  →  VIVS_DE_* register writes
    │
    ├── libdrm_etnaviv.so      ← already present; BO + cmd_stream + submit + fence
    │       │
    │       └── ioctl() on /dev/dri/renderD128
    │                 │
    │                 └── etnaviv.ko  ← patched kernel driver (done)
    │
    └── libdrm.so              ← KMS page-flip on /dev/dri/card0
```

### Do I need a userspace HAL? — Yes, but a thin one.

"HAL" spans two levels. `libdrm_etnaviv` **already is the bottom half**
(BO alloc, `etna_cmd_stream`, relocations, submit ioctl, fence sync — all
proven by `etnaviv_2d_test.c`). What is missing is the **2D op builder**:
functions that translate `fill/blit/blend/stretch` into `VIVS_DE_*`
register sequences + a `DRAW_2D` packet. The `gen_cmd_stream()` in the
test (a hand-written CLEAR) is a slice of exactly this layer.

Build it as a small module `libetna2d` (a few hundred lines):

| Function | DE path | Registers |
|---|---|---|
| `etna2d_fill(dst, rect, color)` | CLEAR | `DEST_CONFIG_COMMAND_CLEAR`, `CLEAR_PIXEL_VALUE32` (done) |
| `etna2d_blit(src, dst, sr, dr, rop)` | BIT_BLT | `SRC_*`, `DEST_CONFIG_COMMAND_BIT_BLT`, `ROP` |
| `etna2d_blend(src, dst, ..., mode)` | BIT_BLT + PE | `ALPHA_CONTROL`, `ALPHA_MODES` |
| `etna2d_stretch(src, dst, ..., f)` | STRETCH_BLT | `STRETCH_FACTOR_LOW/HIGH` |

This is **not** a reimplementation of Mesa or of Vivante's libGAL.

### What is NOT in this path

**Mesa / EGL / GBM / `etnaviv_dri.so`.** Mesa's etnaviv is a Gallium *3D*
driver: it compiles shaders and drives the 3D pipeline (TS/RA/PE-3D). It
does nothing for the 2D DE block, and on GC520 (`PIPE_3D=0`) it won't even
initialize. There is nothing in Mesa to replicate for 2D.

**Vendor `libGAL` / `libdirectfb_gal.so` (in `drivers/ref/GC520L`).**
Do NOT link against it. It depends on Vivante's own `galcore.ko` and a
separate DRM ABI (`gc_hal_drm.h`) that is **incompatible with `etnaviv.ko`**.
Using it would mean discarding the entire etnaviv bring-up. Keep it only as
a *register-sequence reference* — see `driver/dfb/gc_dfb_raster.c` and the
2D API PDFs under `doc/2D/` for how each DE op is programmed.

**Qt5 app sources need zero modification.** The plugin is selected at
runtime: `QT_QPA_PLATFORM=etnaviv2d ./myapp`.

QML/Qt Quick apps use the OpenGL scene graph and bypass QPainter. They
must run with `QT_QUICK_BACKEND=software` to use the raster path.

---

## Roadmap

Four layers, built bottom-up. Each phase is independently verifiable so a
failure is localized instead of tangled inside the Qt plugin.

### Phase 1 — Validate remaining DE operations (offscreen, CPU-verified)

Goal: prove each 2D op produces correct pixels in a BO. Template:
`etnaviv_2d_test.c`; all registers in `state_2d.xml.h`.

- [ ] `etnaviv_bitblt_test.c`: BIT_BLT BO→BO, verify pixels.
      `VIVS_DE_SRC_ADDRESS`, `SRC_STRIDE`, `SRC_CONFIG`, `DEST_ADDRESS`,
      `DEST_CONFIG` with `COMMAND_BIT_BLT`, `VIVS_DE_ROP`.
- [ ] `etnaviv_alpha_test.c`: BIT_BLT + alpha blend.
      `VIVS_DE_ALPHA_CONTROL`, `VIVS_DE_ALPHA_MODES`.
- [ ] `etnaviv_stretch_test.c`: STRETCH_BLT.
      `VIVS_DE_STRETCH_FACTOR_LOW/HIGH`.

### Phase 2 — Package the ops into `libetna2d` (the thin userspace HAL)

- [ ] Fold the validated sequences into `etna2d_{fill,blit,blend,stretch}()`
      over `etna_cmd_stream`. This is the reusable 2D builder both the
      benchmark app and the Qt plugin call. Keep it Qt-independent.

### Phase 3 — KMS scanout milestone (display half, no Qt)  ← NEW, do before Qt

Display backend = **DRM/KMS on `/dev/dri/card0` (ma35), NOT linuxfb/fbdev.**
The ma35 driver advertises `DRIVER_GEM|DRIVER_MODESET|DRIVER_ATOMIC` with
GEM-DMA (CMA, contiguous) dumb buffers + PRIME (`ma35_drm.c:61`), so KMS
gives zero-copy present via page flip. linuxfb would force a CPU memcpy at
present time — re-adding the CPU cost we are trying to remove. Keep linuxfb
only as a bring-up fallback.

Buffer-sharing direction (dictated by the HW): the display is contiguous
with no display-side IOMMU; etnaviv has its own MMU v2. Therefore allocate
the scanout BO on the **display** side and import it into **etnaviv** — not
the reverse:

```
1. drmModeCreateDumbBuffer on card0 (ma35)  → contiguous scanout BO
2. PRIME export → dma-buf fd  (DRM_IOCTL_PRIME_HANDLE_TO_FD)
3. etna_bo_from_dmabuf(dev, fd)             → etnaviv maps it via its MMU
4. 2D engine (renderD128) renders fill/blit/blend INTO that BO
5. drmModeAddFB2 + atomic page flip on card0 → on panel, zero copy
```

- [ ] Prove on-panel output independently of the paint path: dumb buffer →
      PRIME export → `drmModeAddFB2` → `drmModeSetCrtc`/`drmModePageFlip`.
- [ ] Prove the import path: `etna_bo_from_dmabuf` on the exported fd, then
      DE renders into it (stride/tiling/alignment per
      `doc/2D/Vivante.GPU.Alignment.Requirements`).
- [ ] Double-buffer: two scanout BOs, ping-pong on flip, wait flip event.
- [ ] Standalone benchmark app: N sprites/frame, `etna2d_blit`+`blend` vs a
      CPU `memcpy`/software-blend baseline. This is the clean headline
      accel-vs-CPU number (no toolkit noise). Also satisfies the
      "custom weston/DRM app" benchmark goal.

### Phase 4 — Qt5 QPA plugin (KMS-backed, non-GL)

Compose the plugin from two existing pieces, not from scratch:
- raster structure (`QFbScreen`/backing-store pattern) from **`linuxfb`**
  (`qtbase/src/plugins/platforms/linuxfb/`)
- KMS modeset + page flip from **`kmsconvenience`**
  (`qtbase/src/platformsupport/kmsconvenience/`: `QKmsDevice`, `QKmsOutput`)
  — the same plumbing `eglfs_kms` uses, but without GL/GBM/EGL.

- [ ] Backing store = dma-buf scanout BO (Phase 3 flow); present via KMS
      page flip. Do NOT use fbdev.
- [ ] **Primary win — subclass `QRasterPaintEngine`**
      (`<private/qrasterpaintengine_p.h>`) so pixel work moves to the GPU:
  - [ ] `fillRect()` / `drawRects()` → `etna2d_fill`
  - [ ] `drawImage()` / `drawPixmap()` → `etna2d_blit` (+ `etna2d_blend`
        when composition mode ≠ Source)
  - Fall back to `QRasterPaintEngine::drawXxx()` for unsupported ops.
- [ ] Accelerate `flush()` (backing store BO → scanout BO) with `etna2d_blit`.
      NOTE: this alone yields only a few % — by flush time the CPU has
      already rasterized everything. The paint-engine overrides above are
      where the FPS win comes from; do them first, not last.
- [ ] Register under `QT_QPA_PLATFORM=etnaviv2d`.
- [ ] Benchmark vs `linuxfb` CPU baseline on a representative widget app
      (real-world number, to complement the Phase 3 headline number).

### Phase 5 — Optional extensions

- [ ] Alpha blend compositing in the paint engine (`VIVS_DE_ALPHA_MODES`).
- [ ] Stretch blit for scaled `drawPixmap()`.
- [ ] Filter blit (polyphase scaler) for high-quality downscale.
- [ ] YUV → RGB blit path for video surfaces.

---

## Key Reference Files

| File | Purpose |
|---|---|
| `drivers/gpu/drm/etnaviv/` | Working 6.6 etnaviv tree |
| `tests/libdrm/etnaviv_2d_test.c` | Working DE command stream template (CLEAR) |
| `tests/libdrm/state_2d.xml.h` | Complete DE register map (all 2D operations) |
| `etnaviv/etnaviv_drmif.h` | libdrm_etnaviv userspace API (BO/stream/submit/fence) |
| `drivers/ref/GC520L/.../driver/dfb/gc_dfb_raster.c` | Vendor DE op sequences — **reference only** (galcore ABI, do not link) |
| `drivers/ref/GC520L/.../doc/2D/` | Vivante 2D API + alignment PDFs — DE programming reference |
| `qtbase/src/plugins/platforms/linuxfb/` | QPA plugin skeleton to start from |
| `qtbase/src/plugins/platforms/directfb/` | Reference: non-GL 2D QPA plugin |
| `qtbase/src/plugins/platforms/direct2d/` | Reference: QRasterPaintEngine subclass pattern |
