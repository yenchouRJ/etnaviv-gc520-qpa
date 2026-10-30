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

Declare capabilities in the constructor based on what libetna2d provides (`AlphaFillRectCapability` is intentionally NOT declared: the GC520 has no blended fill, so Qt rasters translucent fills itself):

```cpp
EtnaBlittable(etna2d_context *ctx, etna2d_surface *surf)
    : QBlittable(QSize(etna2d_surface_width(surf), etna2d_surface_height(surf)),
          SolidRectCapability |
          SourcePixmapCapability |
          SourceOverPixmapCapability |
          SourceOverScaledPixmapCapability |
          OpacityPixmapCapability)
```

Virtual method mapping (all 6 overrides):

| Override | Implementation | Notes |
|---|---|---|
| `fillRect(rect, color)` *(pure)* | `etna2d_fill(ctx, surf, rect, argb)` | color must be premultiplied (Qt passes premult when format is `ARGB32_Premultiplied`) |
| `alphaFillRect(rect, color, cmode)` | opaque / Source: `etna2d_fill`; else `cpuFillRect()` | not reached in normal use (capability not declared); kept as a safe implementation. Never call the `QBlittable::alphaFillRect` base stub: it only warns and draws nothing |
| `drawPixmap(dstR, pixmap, srcR)` *(pure)* | `etna2d_blit` if sizes equal, `etna2d_stretch` if not | must obtain source `etna2d_surface*` from pixmap's `EtnaBlittablePlatformPixmap`; `cpuDrawPixmap()` if the source is not an EtnaBlittable (e.g. a `QRasterPlatformPixmap`) |
| `drawPixmapOpacity(dstR, pixmap, srcR, cmode, opacity)` | 1:1: `etna2d_blit` (SRC or SRC_OVER); stretch of the full source: `etna2d_stretch`, via the scratch surface + `SRC_OVER` blit when the source is not fully opaque | `OpacityPixmapCapability` is declared only so Qt passes the composition mode (`CompositionMode_Source`). Opacity < 1, sub-rect stretch and other composition modes go to `cpuDrawPixmap()` |
| `doLock()` *(pure)* | `etna2d_surface_map(surf)` → `new QImage(ptr, w, h, stride, Format_ARGB32_Premultiplied)` | stride = `etna2d_surface_stride(surf)` — NOT `w*4`; format must match GC520 premult model |
| `doUnlock()` *(pure)* | `etna2d_surface_cpu_fini(surf)` then `etna2d_finish(ctx)` | WC drain barrier + fence wait so the GPU sees CPU writes before the next op |

**Critical:** `doLock()` wraps the mapped BO as `Format_ARGB32_Premultiplied`.
The GC520 PE blends premultiplied; the CPU raster fallback runs in the same
format; Qt's backing-store composition is consistent. This was verified in
Phase 2/3 and must NOT be changed to `Format_ARGB32` without matching GC520 fixes.

**Qt does not fall back for us once a plugin method is entered.** `QBlitterPaintEngine`
decides GPU-or-raster *before* calling the blittable, from the declared
capabilities and the painter state. After that it assumes the call drew
everything, so a bare `return` (or the base-class `alphaFillRect` stub) leaves
the pixels untouched. Every case the GPU can't do must draw itself:

- `cpuFillRect()` / `cpuDrawPixmap()` paint into the locked BO with a `QPainter`
  on the `doLock()` image (source read first via `toImage()`).
- `etna2d_stretch()` is a point-sampled copy, no alpha. A translucent source is
  stretched into a cached scratch surface (<= 4 Mpixel, grown on demand) and
  blended with `SRC_OVER`; a fully opaque source is stretched directly.
  `EtnaBlittable::isOpaque()` scans the source once (<= 512 Kpixel) and caches
  the answer; it is invalidated by `doLock()` and by any GPU write to the surface.
- Found with the `qt2dbench --matrix` test (34 to 42 QPainter cases, pixel-exact
  vs CPU raster): pixmap with painter opacity < 1, sub-rect stretch and translucent
  fill after `setBrush(solid)` drew nothing; scaled alpha pixmaps ignored alpha;
  all fixed, all cases now match.
- Qt 5.15 limitation, not fixable here: `drawPixmap` / GPU `fillRect` after a
  non-rectangular `setClipPath()` draws nothing (Qt's blitter engine does not
  refresh its clip state when the clip changes). Work-around in the application:
  `painter.save(); painter.restore();` after `setClipPath()`, or `setClipRegion()`.

**Source-pixmap type check in `drawPixmap`:**
```cpp
// Only accelerate if the source is also an EtnaBlittable surface.
// If it's a QRasterPlatformPixmap (e.g. loaded from PNG, not yet uploaded),
// we draw it with cpuDrawPixmap() (a bare return would leave the pixels
// untouched: Qt does not fall back once drawPixmap has been called).
auto *pm = static_cast<QBlittablePlatformPixmap*>(pixmap.handle());
if (pm->classId() != QPlatformPixmap::BlitterClass) { cpuDrawPixmap(dstR, pixmap, srcR, cmode, opacity); return; }
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
| Source pixmap not EtnaBlittable | `drawPixmap` receives a `QRasterPlatformPixmap` (e.g. PNG loaded from disk) | `cpuDrawPixmap()` draws it; optionally upload to GPU surface on first use. `drawImage` never reaches the plugin (always CPU): convert once with `QPixmap::fromImage()` and draw the pixmap |
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

---

## Phase 5 — QPainter matrix: defects, analysis, fixes

Found with `qt2dbench --matrix` (42 cases; each draws the same commands into a QPixmap on the GPU surface and a QImage CPU reference, compares pixels, and reads the plugin debug lines `QT_ETNAVIV2D_DEBUG=1`). Verified on the board at 1024x600 and 1920x1080: all cases bit-exact (max diff <= 1 for blits), no warnings, except the Qt limitation in section 6.
Code: `src/qetnaviv2dblitter.{h,cpp}`.

### 1. How Qt calls the blittable (root cause of most defects)

`QBlitterPaintEngine` decides GPU vs raster **before** calling the plugin, from the declared `Capabilities` and the painter state (brush, pen, transform, clip, composition mode, opacity). Once a blittable method is called Qt assumes the result is drawn. A bare `return`, or the base `QBlittable::alphaFillRect()` stub (only warns "Please implement alphaFillRect function in your platform or remove AlphaFillRectCapability from it"), leaves the pixels untouched. So every case the plugin cannot do on the GPU must draw itself.
Other facts: `drawImage` always goes to raster (no capability check), so a `QImage` should be converted once with `QPixmap::fromImage` to be blitted. A default painter has `NoBrush`, so `fillRect(rect, color)` is not a GPU call; a solid brush qualifies.

### 2. GC520 / libetna2d limits that shape the fixes

Only fill, blit with ROP, blend NONE or SRC_OVER, and stretch. Stretch copies the whole source into a destination rect, point-sampled, **no blending**, no source sub-rect. No blended fill, no global opacity.

### 3. Defects found and fixes

| # | Symptom (old plugin) | Cause | Fix |
|---|---|---|---|
| 1 | `drawPixmap` with painter opacity < 1, and stretch from a source sub-rect: nothing drawn | `drawPixmapOpacity` returned without drawing for cases the GPU cannot do | `cpuDrawPixmap()`: raster engine (`QPainter`) on `lock()`ed destination, source from `pixmap.toImage()`. Also used for non-blittable sources and unsupported composition modes |
| 2 | translucent `fillRect` after `setBrush(solid)`: nothing drawn, warning printed | `AlphaFillRectCapability` declared, `alphaFillRect` was the base stub | capability removed (Qt rasters translucent fills itself); `alphaFillRect()` now calls `cpuFillRect()` (CPU raster) as a safe fallback |
| 3 | `drawPixmap` under `painter.scale(2,2)` of a translucent pixmap: alpha ignored (max diff 19, 16384 px) | stretch does not blend, so the destination was overwritten | stretch into a cached scratch surface, then `etna2d_blit` with SRC_OVER onto the destination. Scratch grows on demand, max 4 Mpixel, shared (GUI thread only); larger draws go to the CPU |
| 4 | `drawPixmap` with `CompositionMode_Source` and an alpha pixmap blended as source-over (max diff 19, 4096 px) | an intermediate build dropped `OpacityPixmapCapability`, so Qt did not pass the composition mode | `OpacityPixmapCapability` stays declared |

### 4. Opaque-source shortcut (performance)

Fix 3 makes every scaled translucent draw two passes (stretch + blend); for an opaque source the plain stretch is already exact. `EtnaBlittable::isOpaque()` scans the surface alpha once and caches it (`m_opaqueKnown`/`m_opaque`); any CPU `lock()` (`doLock`), `fillRect` or draw into the surface invalidates it. Opaque scale stays single pass (about 190 Mpixel/s at 1024x600 full screen, same as before the fix; the first, unconditional two-pass version dropped to 56).

### 5. Hardcoded limits re-checked at 1080p

| Constant | Value | Check at 1920x1080 |
|---|---|---|
| `kScratchMaxPixels` | 4 Mpixel | a full-screen destination is 2.07 Mpixel: fits; scaled alpha 960x540 to 1920x1080 runs on the GPU path (17 fps in a tight loop) |
| `kOpaqueScanMaxPixels` | was 512 Kpixel, now 4 Mpixel | **512 K was wrong at 1080p**: the bench source (960x540 = 518 K) fits only by chance; an opaque 1000x600 source skipped the scan and took the two-pass path: 16.7 fps vs 56 fps with the scan (CPU raster 58 fps). Raised to 4 M. Cost: if an opaque source is rewritten every frame the scan repeats (6.4 fps in that synthetic loop) |

Limits for open items (not fixed): scaled alpha draws larger than the screen surface (e.g. 960x540 to 2400x1200 on a 1080p surface) run at about 2 fps with or without the scratch cap, not investigated; translucent `fillRect` through the plugin is slower than on linuxfb (13 vs 31 Mpixel/s offscreen at 1080p) because Qt rasters it on the uncached buffer.

### 6. Qt 5.15 limitation (not fixable in the plugin)

After `setClipPath()` with a non-rectangular path, `drawPixmap` (and GPU `fillRect`) on a QPixmap painter draws nothing (matrix cases "ellipse clip" and "ellipse clip (AA hint)": 3220 / 3290 px differ). `QBlitterPaintEnginePrivate::updateClipState()` runs only from `clipEnabledChanged()` and `setState()`, not when the clip changes, so the `STATE_CLIP_COMPLEX` bit is stale; Qt takes the GPU branch, finds neither a rect nor a region clip and skips the draw without calling the plugin.
Bisected with extra matrix cases: `drawImage` + ellipse clip matches; ellipse clip + `save()/restore()` matches; `setClipRegion` matches; rect-path clip matches. Work-arounds: `painter.save(); painter.restore();` after `setClipPath()`, or `setClipRegion()`. The proper fix is in Qt (`qpaintengine_blitter.cpp`).

### 7. Procedure

Build in the Buildroot tree (`make` in the plugin build dir), copy `libqetnaviv2d.so` to `/usr/lib/qt/plugins/platforms/` on the board, stop weston (it holds DRM master), run `qt2dbench --matrix` (and `--clip`, `--ops`) from a writable directory with `QT_QPA_PLATFORM=etnaviv2d:plane=primary`.
