# GC520 Qt5 2D Acceleration — Project Summary

Target: Nuvoton MA35D1 SoC, Vivante GC520 (2D-only GPU), Linux 6.6, Qt5.

Buildroot:
- /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
drivers:
- {buildroot}/output/build/linux-custom/drivers/gpu/drm
devicetree:
- {buildroot}/output/build/linux-custom/arch/arm64/boot/dts/nuvoton/ma35d1.dtsi
- {buildroot}/output/build/linux-custom/arch/arm64/boot/dts/nuvoton/ma35d1-som-256m.dts
packages:
- {buildroot}/output/build/libdrm-2.4.120
- {buildroot}/output/build/qt5base-

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
Custom Qt5 QPA plugin          ← new work
    │
    ├── libdrm_etnaviv.so      ← already present; deps: libdrm + libc only
    │       │
    │       └── ioctl() on /dev/dri/renderD128
    │                 │
    │                 └── etnaviv.ko  ← patched kernel driver (done)
    │
    └── libdrm.so              ← KMS page-flip on /dev/dri/card0
```

**Mesa, EGL, GBM, `etnaviv_dri.so` are not in this path.**
`etnaviv_dri.so` (Mesa Gallium) opens `ETNA_PIPE_3D` — it fails on GC520
and is irrelevant. The custom plugin talks directly to `libdrm_etnaviv`
with `ETNA_PIPE_2D`.

**Qt5 app sources need zero modification.** The plugin is selected at
runtime: `QT_QPA_PLATFORM=etnaviv2d ./myapp`.

QML/Qt Quick apps use the OpenGL scene graph and bypass QPainter. They
must run with `QT_QUICK_BACKEND=software` to use the raster path.

---

## Roadmap

### Phase 1 — Validate remaining DE operations (before Qt5 work)

- [ ] Write `etnaviv_bitblt_test.c`: BIT_BLT from one BO to another, verify pixels.
      Template: `etnaviv_2d_test.c`. Key registers: `VIVS_DE_SRC_ADDRESS`,
      `VIVS_DE_SRC_STRIDE`, `VIVS_DE_SRC_CONFIG`, `VIVS_DE_DEST_ADDRESS`,
      `VIVS_DE_DEST_CONFIG` with `COMMAND_BIT_BLT`, `VIVS_DE_ROP`.
- [ ] Write `etnaviv_alpha_test.c`: BIT_BLT + alpha blend. Key registers:
      `VIVS_DE_ALPHA_CONTROL`, `VIVS_DE_ALPHA_MODES`.
- [ ] Write `etnaviv_stretch_test.c`: STRETCH_BLT with `VIVS_DE_STRETCH_FACTOR_LOW/HIGH`.

All register definitions are in `state_2d.xml.h`. Working command stream
pattern is in `etnaviv_2d_test.c`.

### Phase 2 — Qt5 QPA plugin (new project)

- [ ] Create plugin skeleton based on `qlinuxfb` source
      (`qtbase/src/plugins/platforms/linuxfb/`).
- [ ] Replace `flush()` memcpy with GC520 DE BIT_BLT (backing store BO → KMS BO).
      This alone accelerates the final screen blit for all QWidget apps.
- [ ] Subclass `QRasterPaintEngine` (private API: `<private/qrasterpaintengine_p.h>`):
  - [ ] Override `drawRects()` → DE CLEAR command
  - [ ] Override `drawPixmap()` → DE BIT_BLT command
  - [ ] Override `drawImage()` → DE BIT_BLT command
  - Fallback to `QRasterPaintEngine::drawXxx()` for unsupported ops.
- [ ] KMS integration: allocate framebuffer BOs exportable as dma-buf,
      import into display controller via `drmPrimeFDToHandle` + `drmModeAddFB2`
      + `drmModePageFlip`.
- [ ] Register plugin under `QT_QPA_PLATFORM=etnaviv2d`.
- [ ] Benchmark frame rate vs linuxfb CPU baseline on a representative widget app.

### Phase 3 — Optional extensions

- [ ] Alpha blend compositing in the paint engine (`VIVS_DE_ALPHA_MODES`).
- [ ] Stretch blit for scaled `drawPixmap()`.
- [ ] Filter blit (polyphase scaler) for high-quality downscale.
- [ ] YUV → RGB blit path for video surfaces.

---

## Key Reference Files

| File | Purpose |
|---|---|
| `drivers/gpu/drm/etnaviv/` | Working 6.6 etnaviv tree |
| `tests/etnaviv/etnaviv_2d_test.c` | Working DE command stream template |
| `tests/etnaviv/state_2d.xml.h` | Complete DE register map (all 2D operations) |
| `etnaviv/etnaviv_drmif.h` | Full libdrm_etnaviv userspace API |
| `qtbase/src/plugins/platforms/linuxfb/` | QPA plugin skeleton to start from |
| `qtbase/src/plugins/platforms/directfb/` | Reference: non-GL 2D QPA plugin |
| `qtbase/src/plugins/platforms/direct2d/` | Reference: QRasterPaintEngine subclass pattern |
