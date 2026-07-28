# etnaviv2d QPA Plugin — Implementation Plan

Target: Qt 5.15, Nuvoton MA35D1, GC520 2D GPU, KMS/DRM display.
Plugin name: `qetnaviv2d` — selected with `QT_QPA_PLATFORM=etnaviv2d`.

---

## Architecture Overview

```
QPainter API (unchanged)
    │
    ▼
QBlitterPaintEngine  (Qt-provided, qpaintengine_blitter_p.h)
    │  HW-accelerated op?  →  QBlittable virtuals  →  libetna2d
    │  CPU fallback?        →  QRasterPaintEngine renders into lock()-ed BO memory
    ▼
EtnaBlittable  (ours, ~150 lines)
    │
    ├── etna2d_fill / etna2d_blit / etna2d_blend_over / etna2d_stretch
    │
    ▼
libetna2d   (done — hal/libetna2d/)
    │
    ├── libdrm_etnaviv.so  (renderD128)
    └── libdrm.so          (card0, KMS page-flip)
```

Qt already provides the paint-engine and dispatch machinery via the
`QBlittable` architecture (used by the `directfb` plugin). We implement one
HW-hook class; Qt handles QPainter dispatch and the automatic raster fallback.
**Do not subclass QRasterPaintEngine directly.**

Primary reference: `src/plugins/platforms/directfb/qdirectfbblitter.{h,cpp}`

---

## File Structure

```
qpa/
├── plan.md                          ← this file
├── etnaviv2d.pro                    ← qmake plugin build
├── etnaviv2d.json                   ← plugin metadata
├── main.cpp                         ← QPA plugin factory entry point
│
├── qetnaviv2dintegration.h/.cpp     ← QPlatformIntegration subclass
│                                       (createPlatformPixmap, createPlatformBackingStore,
│                                        createPlatformWindow, createEventDispatcher, ...)
├── qetnaviv2dscreen.h/.cpp          ← QFbScreen subclass; owns KMS device + scanout BOs;
│                                       handles modeset, page-flip, vblank
├── qetnaviv2dblitter.h/.cpp         ← EtnaBlittable  : QBlittable
│                                       EtnaBlittablePlatformPixmap : QBlittablePlatformPixmap
└── qetnaviv2dbackingstore.h/.cpp    ← QPlatformBackingStore; paintDevice() returns QPixmap
                                        over blittable; flush() → etna2d_blit + KMS flip
```

---

## Component Details

### 1. `EtnaBlittable : QBlittable`  — `qetnaviv2dblitter.h/.cpp`

The single hardware-hook class. Constructor takes an `etna2d_surface*` (already
allocated by the caller — either a fresh `etna2d_surface_create` or imported via
`etna2d_surface_from_dmabuf`).

Declare capabilities in the constructor based on what libetna2d provides:

```cpp
EtnaBlittable(etna2d_context *ctx, etna2d_surface *surf)
    : QBlittable(QSize(etna2d_surface_width(surf), etna2d_surface_height(surf)),
          SolidRectCapability |
          AlphaFillRectCapability |
          SourcePixmapCapability |
          SourceOverPixmapCapability |
          SourceOverScaledPixmapCapability |
          OpacityPixmapCapability)
```

Virtual method mapping (all 6 overrides):

| Override | Implementation | Notes |
|---|---|---|
| `fillRect(rect, color)` *(pure)* | `etna2d_fill(ctx, surf, rect, argb)` | color must be premultiplied (Qt passes premult when format is `ARGB32_Premultiplied`) |
| `alphaFillRect(rect, color, cmode)` | `etna2d_fill(ctx, surf, rect, argb_premult)` | only handle `CompositionMode_SourceOver`; else call `QBlittable::alphaFillRect` |
| `drawPixmap(dstR, pixmap, srcR)` *(pure)* | `etna2d_blit` if sizes equal, `etna2d_stretch` if not | must obtain source `etna2d_surface*` from pixmap's `EtnaBlittablePlatformPixmap`; fallback to raster if source is not an EtnaBlittable (e.g. a `QRasterPlatformPixmap`) |
| `drawPixmapOpacity(dstR, pixmap, srcR, cmode, opacity)` | premultiply + `etna2d_blend_over` or `etna2d_stretch` | opacity: premultiply source alpha by `opacity` before blending; note GC520 PE blends premultiplied |
| `doLock()` *(pure)* | `etna2d_surface_map(surf)` → `new QImage(ptr, w, h, stride, Format_ARGB32_Premultiplied)` | stride = `etna2d_surface_stride(surf)` — NOT `w*4`; format must match GC520 premult model |
| `doUnlock()` *(pure)* | `etna2d_surface_cpu_fini(surf)` then `etna2d_finish(ctx)` | WC drain barrier + fence wait so the GPU sees CPU writes before the next op |

**Critical:** `doLock()` wraps the mapped BO as `Format_ARGB32_Premultiplied`.
The GC520 PE blends premultiplied; the CPU raster fallback runs in the same
format; Qt's backing-store composition is consistent. This was verified in
Phase 2/3 and must NOT be changed to `Format_ARGB32` without matching GC520 fixes.

**Source-pixmap type check in `drawPixmap`:**
```cpp
// Only accelerate if the source is also an EtnaBlittable surface.
// If it's a QRasterPlatformPixmap (e.g. loaded from PNG, not yet uploaded),
// we fall through to the raster fallback (Qt handles this automatically
// because we don't call any etna2d op — Qt's QBlitterPaintEngine will lock
// us and raster-blit).
auto *pm = static_cast<QBlittablePlatformPixmap*>(pixmap.handle());
if (pm->classId() != QPlatformPixmap::BlitterClass) { /* fallback */ return; }
auto *src = static_cast<EtnaBlittablePlatformPixmap*>(pm)->etnaBlittable()->surface();
```

### 2. `EtnaBlittablePlatformPixmap : QBlittablePlatformPixmap`  — same files

Single method:
```cpp
QBlittable *createBlittable(const QSize &size, bool alpha) const override
{
    etna2d_surface *surf = nullptr;
    etna2d_surface_create(g_dev, size.width(), size.height(), &surf);
    return new EtnaBlittable(g_ctx, surf);
}
```
`g_dev` / `g_ctx` are owned by the integration singleton; expose via a global
or pass through a factory pointer.

Qt's `QBlittablePlatformPixmap` base supplies:
- `paintEngine()` → `QBlitterPaintEngine` (automatic)
- `buffer()` → `blittable()->lock()` (automatic)
- `fromImage()` — copies QImage into the blittable via CPU path (automatic)
- `classId()` → `BlitterClass` (automatic)

### 3. `QEtnaviv2dIntegration : QPlatformIntegration`  — `qetnaviv2dintegration.h/.cpp`

Modelled on `QLinuxFbIntegration` (linuxfb) + `QDirectFbIntegration`.

Init sequence in constructor (order matters):
1. Open render node: `etna2d_device_open("/dev/dri/renderD128", &g_dev)`
2. Create context: `etna2d_context_create(g_dev, &g_ctx)`
3. Open KMS: `open("/dev/dri/card0", ...)` — see screen below
4. Create `QEtnaviv2dScreen`
5. `QWindowSystemInterface::handleScreenAdded(screen)`
6. Input: `new QEvdevKeyboardManager(...)` / `QEvdevMouseManager` / `QEvdevTouchManager`
7. Font DB: `new QGenericUnixFontDatabase()`

Required overrides (copy signatures from directfb/linuxfb integration):

```cpp
bool hasCapability(Capability cap) const override;
    // ThreadedPixmaps: true; ScreenCount: n/a; OpenGL: false

QPlatformPixmap *createPlatformPixmap(QPlatformPixmap::PixelType type) const override;
    // BitmapType → QRasterPlatformPixmap; else → EtnaBlittablePlatformPixmap

QPlatformBackingStore *createPlatformBackingStore(QWindow *w) const override;
    // → new QEtnaviv2dBackingStore(w)

QPlatformWindow *createPlatformWindow(QWindow *w) const override;
    // → new QFbWindow(w)   (QFbScreen manages the window stack)

QAbstractEventDispatcher *createEventDispatcher() const override;
    // → createUnixEventDispatcher()

QPlatformFontDatabase *fontDatabase() const override;
QPlatformServices *services() const override;
QPlatformInputContext *inputContext() const override;
QList<QPlatformScreen*> screens() const override;
```

### 4. `QEtnaviv2dScreen : QFbScreen`  — `qetnaviv2dscreen.h/.cpp`

Owns the KMS display side. Reuses our Phase 3 standalone card0 code
(`hal/kms/`): dumb buffer → PRIME → `drmModeAddFB2` → page-flip.

**Do NOT use `QKmsDevice`** — it is eglfs-oriented (GBM buffers); our Phase 3
KMS code is a drop-in and already hardware-verified.

Responsibilities:
- `initialize()`: open `card0`, `drmModeGetResources`, pick connector + CRTC,
  allocate two dumb BOs (double-buffer), PRIME export each, `drmModeAddFB2`,
  initial `drmModeSetCrtc` for mode set.
- Expose `scanoutSurface(int idx)` → the `etna2d_surface*` imported from that
  dumb BO's dma-buf (via `etna2d_surface_from_dmabuf`). Used by backing store.
- `pageFlip(int fb_id)` + vblank wait (non-blocking flip + `drmHandleEvent`).
- Override `doRedraw()` from `QFbScreen` — in our model doRedraw is suppressed;
  the backing store's `flush()` drives the flip directly.
- Store: `mGeometry` (from drm mode), `mDepth = 32`, `mFormat = Format_ARGB32_Premultiplied`.
- Physical size: derive from DRM connector mm_width/mm_height, or a fallback (96 dpi).

Double-buffer state:
```
scanout_bo[2]   — dumb BOs on card0
scanout_surf[2] — etna2d_surface* imported from dma-buf of each BO
scanout_fb[2]   — DRM FB ids from drmModeAddFB2
front_idx       — which is currently displayed
```

### 5. `QEtnaviv2dBackingStore : QPlatformBackingStore`  — `qetnaviv2dbackingstore.h/.cpp`

Modelled on `QDirectFbBackingStore`. This is the v1 **offscreen BO + blit on flush (option B)** design.

```
m_pixmap      — QPixmap wrapping the offscreen EtnaBlittablePlatformPixmap
m_pmdata      — EtnaBlittablePlatformPixmap* (backing-store offscreen surface)
```

Required overrides:

```cpp
QPaintDevice *paintDevice() override { return &m_pixmap; }

void resize(const QSize &size, const QRegion &staticContents) override;
    // etna2d_surface_destroy old, etna2d_surface_create new, rebuild m_pixmap

void flush(QWindow *window, const QRegion &region, const QPoint &offset) override;
    // 1. m_pmdata->blittable()->unlock()   (flush any pending CPU writes + fence)
    // 2. etna2d_blit(ctx, backing_surf, scanout_surf[back_idx], ...)
    //    blit only the dirty region for efficiency
    // 3. etna2d_finish(ctx)                (fence: GPU done before flip)
    // 4. screen->pageFlip(scanout_fb[back_idx])
    // 5. swap front_idx / back_idx

QImage toImage() const override;
    // return QImage over the locked backing surface (for screenshot/debug)

bool scroll(const QRegion &area, int dx, int dy) override;
    // etna2d_blit within the backing surface — optional, skip for v1
```

---

## Build System — `etnaviv2d.pro`

```qmake
TARGET = qetnaviv2d

DEFINES += QT_NO_FOREACH

QT += \
    core-private \
    gui-private \
    service_support-private \
    eventdispatcher_support-private \
    fontdatabase_support-private \
    fb_support-private

# Input support (optional — same guard as linuxfb)
qtHaveModule(input_support-private): QT += input_support-private

SOURCES = \
    main.cpp \
    qetnaviv2dintegration.cpp \
    qetnaviv2dscreen.cpp \
    qetnaviv2dblitter.cpp \
    qetnaviv2dbackingstore.cpp

HEADERS = \
    qetnaviv2dintegration.h \
    qetnaviv2dscreen.h \
    qetnaviv2dblitter.h \
    qetnaviv2dbackingstore.h

# libetna2d (our userspace HAL)
INCLUDEPATH += $$PWD/../hal/libetna2d
LIBS += -L$$PWD/../hal/libetna2d/build -letna2d

# libdrm + libdrm_etnaviv (already present in sysroot)
LIBS += -ldrm -ldrm_etnaviv

OTHER_FILES += etnaviv2d.json

PLUGIN_TYPE = platforms
PLUGIN_CLASS_NAME = QEtnaviv2dIntegrationPlugin
!equals(TARGET, $$QT_DEFAULT_QPA_PLUGIN): PLUGIN_EXTENDS = -
load(qt_plugin)
```

---

## `etnaviv2d.json`

```json
{ "Keys": [ "etnaviv2d" ] }
```

---

## Implementation Order (bottom-up, each independently testable)

### Step 1 — `EtnaBlittable` unit test (no display)
- Implement `EtnaBlittable` + `EtnaBlittablePlatformPixmap`.
- Standalone C++ test: create a `QPixmap` from `EtnaBlittablePlatformPixmap`,
  get a `QPainter`, call `fillRect` / `drawPixmap` / `drawImage`.
- Read back via `pixmap.toImage()` and compare to CPU reference.
- **Verify**: HW ops execute, premult format is preserved, raster fallback works
  (test with a `QRasterPlatformPixmap` source — should not crash).

### Step 2 — Screen + modeset (no Qt painting)
- Implement `QEtnaviv2dScreen::initialize()`: KMS open, mode set, dumb BOs,
  PRIME, `etna2d_surface_from_dmabuf`.
- Test: fill `scanout_surf[0]` with `etna2d_fill` (solid color), page-flip →
  should show on panel (mirrors Phase 3 `kms_gpu_test`).
- **Verify**: both scanout surfaces importable, flip+vblank works.

### Step 3 — Integration + backing store (first Qt window on panel)
- Wire `QEtnaviv2dIntegration` + `QEtnaviv2dBackingStore`.
- Run a minimal Qt app: `QWidget` with a `paintEvent` that calls `fillRect`.
- **Verify**: widget appears on panel, resize works, no GPU faults.

### Step 4 — End-to-end widget benchmark
- Run a representative widget app (buttons, labels, icons) via
  `QT_QPA_PLATFORM=etnaviv2d` and benchmark FPS vs `QT_QPA_PLATFORM=linuxfb`.
- Expected bottleneck: text rendering (glyph fallback to raster), not fill/blit.

---

## Known Risks & Mitigations

| Risk | Details | Mitigation |
|---|---|---|
| Per-op fixed cost (~2.5–3 µs) | DE reloads full state per op; widget UIs issue many tiny fills | Defer `etna2d_flush` to `flush()`/`doUnlock()` only (batch in ctx buffer); don't flush per-op |
| Glyph rendering is CPU | `drawCachedGlyphs` not implemented; all text is raster fallback | Accept for v1; raster fallback works correctly via `doLock()`; add glyph path in Phase 5 |
| Source pixmap not EtnaBlittable | `drawPixmap` receives a `QRasterPlatformPixmap` (e.g. PNG loaded from disk) | Fall back to raster (Qt handles this); optionally upload to GPU surface on first use |
| WC read performance | CPU raster fallback reads from WC-mapped BO memory (uncached reads are slow) | Acceptable for fallback path; profile if text-heavy UIs are too slow |
| `etna2d_stretch_batch` not exposed through QBlittable | `QBlittable::drawPixmap` is per-rect; no batch hook | Accept for v1; the per-op cost is ~3µs so stretch tiles must be large enough (≥16px) to win |
| KMS threading | `flush()` may be called from Qt's render thread | Card0 fd and flip state must stay on one thread; use a mutex or post flip to main thread |

---

## File Dependencies at a Glance

```
qetnaviv2dblitter.{h,cpp}
    → QBlittable           (private/qblittable_p.h)
    → QBlittablePlatformPixmap (private/qpixmap_blitter_p.h)
    → etna2d.h             (hal/libetna2d/etna2d.h)

qetnaviv2dscreen.{h,cpp}
    → QFbScreen            (fb_support-private)
    → libdrm / xf86drmMode (KMS ioctls — Phase 3 code)
    → etna2d.h             (etna2d_surface_from_dmabuf)

qetnaviv2dbackingstore.{h,cpp}
    → QPlatformBackingStore (qpa/qplatformbackingstore.h)
    → QBlittablePlatformPixmap
    → qetnaviv2dblitter.h
    → qetnaviv2dscreen.h

qetnaviv2dintegration.{h,cpp}
    → QPlatformIntegration
    → qetnaviv2dscreen.h
    → qetnaviv2dblitter.h
    → qetnaviv2dbackingstore.h
    → QFbWindow            (fb_support-private)
    → QGenericUnixFontDatabase (fontdatabase_support-private)
    → QEvdev*Manager       (input_support-private)
```
