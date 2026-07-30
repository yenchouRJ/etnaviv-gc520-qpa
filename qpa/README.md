# etnaviv2d QPA Plugin — Build & Test Guide

Qt 5.15 QPA platform plugin for the Vivante GC520 2D GPU on Nuvoton MA35D1.
Accelerates QPainter fill/blit/blend/stretch via the GC520 DE through libetna2d.

---

## Repository layout

```
qpa/
├── README.md          this file
├── plan.md            design decisions and architecture
├── etnaviv2d.pro      qmake plugin build file
├── Makefile           top-level build orchestrator
├── src/               plugin source (synced into Qt build tree to compile)
│   ├── main.cpp
│   ├── qetnaviv2dblitter.h/.cpp       EtnaBlittable — HW op hook
│   ├── qetnaviv2dscreen.h/.cpp        KMS screen + page-flip
│   ├── qetnaviv2dbackingstore.h/.cpp  backing store + flush
│   └── qetnaviv2dintegration.h/.cpp   QPlatformIntegration glue
└── tests/
    ├── test_blitter/      Step 1 — GPU ops + pixel readback (no display needed)
    ├── test_screen/       Step 2 — KMS modeset + colour flip on panel
    ├── test_integration/  Step 3 — first QWidget on panel via the plugin
    └── test_benchmark/    Step 4 — FPS benchmark vs linuxfb CPU baseline
```

---

## Why the plugin builds in-tree

Qt platform plugins use private Qt headers (`qblittable_p.h`, `qfbscreen_p.h`,
etc.) and the `load(qt_plugin)` qmake macro that requires `.qmake.conf` from
the Qt build tree.  The host wrapper binary at `output/host/bin/qmake` does
**not** know the build tree location and fails with:

```
Project ERROR: Project has no top-level .qmake.conf file.
```

The fix is to use the **in-tree qmake** at `$QT_BUILD/bin/qmake` and to run it
from inside `$QT_BUILD/src/plugins/platforms/etnaviv2d/` — exactly the same
way `linuxfb`, `eglfs`, and every other Qt plugin is built.

`make plugin` automates this: it rsyncs `src/` into the Qt build tree then
invokes the in-tree qmake.  The test binaries also use the in-tree qmake (they
need `gui-private` and `fb_support-private` which the wrapper qmake can't
resolve).

---

## Prerequisites

### Host (build machine)

| Item | Where it comes from |
|---|---|
| Buildroot workspace | `ma35d1_br24_qt5` fully built (`make all` completed) |
| In-tree qmake | `$QT_BUILD/bin/qmake` (present after Qt build) |
| aarch64 cross-toolchain | `$SYSROOT/bin/aarch64-nuvoton-linux-gnu-g++` |
| libetna2d | built from `../hal/libetna2d/` (see below) |
| rsync | standard Linux package (`apt install rsync`) |

Where:
```
QT_BUILD = output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
SYSROOT  = output/host
```
(relative to the Buildroot workspace root
`/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/`)

### Target (MA35D1 board)

The following must already be present in the rootfs:

| File | Path on target |
|---|---|
| Qt platform plugins | `/usr/lib/qt/plugins/platforms/` |
| Qt shared libs | `/usr/lib/libQt5*.so*` |
| libdrm / libdrm_etnaviv | `/usr/lib/libdrm*.so*` |
| etnaviv kernel module | loaded (`modprobe etnaviv` or built-in) |
| DRM devices | `/dev/dri/card0`, `/dev/dri/renderD128` |
| libetna2d | `/usr/lib/libetna2d.so.1` (copy from build, see below) |

---

## Build

### 1. Build libetna2d (once)

```sh
cd /home/joeylu/opencode/graphic/drm-2d-gc520/hal/libetna2d
make
# produces: build/libetna2d.a  build/libetna2d.so  build/libetna2d.so.1
```

### 2. Build the plugin

```sh
cd /home/joeylu/opencode/graphic/drm-2d-gc520/qpa
make plugin
```

What `make plugin` does:
1. Creates `$QT_BUILD/src/plugins/platforms/etnaviv2d/`
2. `rsync`s `src/` and `etnaviv2d.pro` into that directory
3. Runs `$QT_BUILD/bin/qmake etnaviv2d.pro` from inside that directory
4. Runs `make` — cross-compiles with aarch64 toolchain
5. Installs the `.so` to `$QT_BUILD/plugins/platforms/libqetnaviv2d.so`

### 3. Build the tests

```sh
make tests          # all four tests
make test_blitter   # individual target
make test_screen
make test_integration
make test_benchmark
```

Test binaries land in `build/test_*/`.

### 4. Build everything at once

```sh
make all
```

### Override paths (if your workspace differs)

```sh
make all \
    QT_BUILD=/path/to/qt5base-build \
    ETNA2D_LIBDIR=/path/to/libetna2d/build \
    ETNA2D_INCDIR=/path/to/libetna2d \
    KMS_INCDIR=/path/to/hal/kms
```

### Clean

```sh
make clean          # removes qpa/build/ only
# To also remove the in-tree plugin:
rm -rf $QT_BUILD/src/plugins/platforms/etnaviv2d
```

---

## Deploy to target

Copy the plugin, tests, and libetna2d to the board (adjust IP/path):

```sh
BOARD=root@192.168.1.100
QT_BUILD=/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
QPA_DIR=/home/joeylu/opencode/graphic/drm-2d-gc520/qpa

# Plugin
scp $QT_BUILD/plugins/platforms/libqetnaviv2d.so \
    $BOARD:/usr/lib/qt/plugins/platforms/

# libetna2d shared library
scp $QPA_DIR/../hal/libetna2d/build/libetna2d.so.1 \
    $BOARD:/usr/lib/
ssh $BOARD "ln -sf libetna2d.so.1 /usr/lib/libetna2d.so"

# Test binaries
scp $QPA_DIR/build/test_blitter/test_blitter   $BOARD:/tmp/
scp $QPA_DIR/build/test_screen/test_screen     $BOARD:/tmp/
scp $QPA_DIR/build/test_integration/test_integration $BOARD:/tmp/
scp $QPA_DIR/build/test_benchmark/test_benchmark     $BOARD:/tmp/
```

---

## Test on the target board

Run the four steps in order.  Each step validates one layer independently so a
failure is easy to localise.

### Step 1 — EtnaBlittable unit test (no display required)

Validates the GPU blitter layer: fill, blit, stretch, and raster fallback.
Reads pixels back via `toImage()` and compares against expected values.

```sh
# On the board:
QT_ETNAVIV2D_DEBUG=1 /tmp/test_blitter /dev/dri/renderD128
```

Expected output:
```
[ OK ] GPU device/context ready
[ OK ] fillRect: red pixel at centre
[ OK ] fillRect: blue sub-rect
[ OK ] fillRect: area outside sub-rect unchanged
[ OK ] drawPixmap: centre of blit region
[ OK ] drawPixmap: outside blit region unchanged
[ OK ] stretch TL=red
[ OK ] stretch TR=green
[ OK ] stretch BL=blue
[ OK ] stretch BR=yellow
[ OK ] raster fallback: text produces non-white pixels
[ OK ] All blitter tests PASSED
```

If this step fails, check that `renderD128` is accessible and etnaviv module is loaded.

### Step 2 — KMS screen test (display required)

Validates modeset, double-buffer allocation, PRIME import into etnaviv, and
page-flip.  Watch the panel.

```sh
QT_ETNAVIV2D_DEBUG=1 /tmp/test_screen /dev/dri/card0 /dev/dri/renderD128
```

Expected behaviour on panel:
- **Red** fullscreen for 1 second
- **Green** fullscreen for 1 second
- **Blue** fullscreen for 1 second
- Original CRTC restored on exit

Expected terminal output:
```
[ OK ] GPU device/context ready
[ OK ] QEtnaviv2dScreen::initialize()
[ OK ] Screen geometry: 1024x600
[ OK ] Physical size: ...
[ OK ] screen width > 0
[ OK ] screen height > 0
  Filling red (1 s)...
[ OK ] etna2d_finish red
[ OK ] pageFlip red
  ...
[ OK ] All screen tests PASSED
```

If `pageFlip` fails, check that no other process holds `/dev/dri/card0`
(e.g. kill any running `linuxfb` Qt app first).

### Step 3 — First Qt window on panel

Validates the full plugin: `QWidget` paintEvent, backing store, flush, and
page-flip.  The plugin is loaded via `QT_QPA_PLATFORM`.

```sh
QT_QPA_PLATFORM=etnaviv2d \
QT_ETNAVIV2D_DEBUG=1 \
/tmp/test_integration
```

Expected panel output (3 seconds, then auto-close):
- Dark grey background
- Red rectangle top-left, green top-right, blue bottom-left
- Quadrant colour pattern (orange/yellow/cyan/magenta) bottom-right (stretched)
- White text "etnaviv2d OK" in the centre (rendered via CPU raster fallback)

Expected terminal:
```
[ OK ] QApplication created (plugin: etnaviv2d)
[ OK ] source pixmap prepared
[ OK ] paintEvent executed
[ OK ] integration test complete
```

With optional parameters:
```sh
QT_QPA_PLATFORM="etnaviv2d:card=/dev/dri/card0:render=/dev/dri/renderD128" \
/tmp/test_integration
```

### Step 4 — Benchmark vs linuxfb

Measures GPU-accelerated FPS against the CPU raster baseline.
Run the same binary twice with different `QT_QPA_PLATFORM` values.

```sh
# GPU (etnaviv2d)
QT_QPA_PLATFORM=etnaviv2d /tmp/test_benchmark 120 32
# → prints: platform=etnaviv2d  frames=120  sprites=32  elapsed=...  fps=XX.X

# CPU baseline (linuxfb)
QT_QPA_PLATFORM=linuxfb /tmp/test_benchmark 120 32
# → prints: platform=linuxfb    frames=120  sprites=32  elapsed=...  fps=XX.X
```

Parameters: `test_benchmark [nframes] [nsprites]`
- `nframes` — number of frames to render (default 120 ≈ 2 s at 60 Hz)
- `nsprites` — number of 96×96 sprites composited per frame (default 32)

Expected: GPU FPS should exceed CPU FPS for sprite-heavy scenes.
Text-heavy scenes may be similar (text is raster fallback on both paths).

---

## Environment variables

| Variable | Effect |
|---|---|
| `QT_ETNAVIV2D_DEBUG=1` | Enable verbose logging from all plugin layers |
| `QT_QPA_PLATFORM=etnaviv2d` | Select this plugin |
| `QT_QPA_PLATFORM="etnaviv2d:card=/dev/dri/card0:render=/dev/dri/renderD128"` | Override device paths |
| `QT_QPA_PLATFORM="etnaviv2d:ctx=1024"` | Override context op-buffer size (default 512) |
| `QT_QPA_ETNAVIV2D_DISABLE_INPUT=1` | Skip evdev input handler creation |
| `QT_QPA_ETNAVIV2D_NO_LIBINPUT=1` | Force evdev even if libinput is available |
| `QT_ETNAVIV2D_NO_BATCH=1` | Disable source-pixmap batch unlock. By default (0) the plugin defers `etna2d_finish()` to frame boundary — source pixmaps are released with a WC-drain only (`cpu_fini`), keeping all per-frame GPU ops in one command-stream submission. Set to 1 to revert to a conservative full `finish()` on every `drawPixmap` source surface; useful for debugging GPU ordering issues or measuring the baseline cost of per-op serialisation. |

---

## Troubleshooting

**`etnaviv2d: etna2d_device_open failed`**
- Check `/dev/dri/renderD128` exists and is readable.
- `lsmod | grep etnaviv` — module must be loaded.
- `dmesg | grep etnaviv` — look for initialisation errors.

**`etnaviv2d: QEtnaviv2dScreen::initialize() failed`**
- Check `/dev/dri/card0` is accessible and not held by another process.
- `dmesg | grep ma35\|drm` — look for KMS errors.

**Blank panel after modeset**
- GPU fence timeout: check `dmesg` for MMU faults or GPU hangs.
- `etna2d_finish` may be blocking indefinitely — GPU may have hung.

**Black screen / no flip**
- Another process may own the CRTC. Kill any `linuxfb` Qt processes first.

**Plugin not found at runtime**
- `QT_PLUGIN_PATH=/usr/lib/qt/plugins QT_QPA_PLATFORM=etnaviv2d ./myapp`
- Or copy `libqetnaviv2d.so` to the same directory as your app binary.

**Slow performance / lower FPS than linuxfb**
- With many small widgets (buttons, labels), per-op DE overhead (~2.5 µs) may
  dominate. This is expected for text-heavy UIs where most ops are raster
  fallback. See `plan.md` — glyph acceleration is Phase 5.
