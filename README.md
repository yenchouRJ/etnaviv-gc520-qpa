# drm-2d-gc520

Zero-copy 2D graphics + video stack for the Vivante GC520 2D GPU on the
Nuvoton MA35D1, built bottom-up from raw DRM/KMS + a custom hardware
abstraction library, through a Qt5 QPA platform plugin, up to a full
Weston/Wayland compositor with hardware-cursor, dual-client (Qt UI + video)
plane arbitration, and ivi-shell layer management.

No GL/Vulkan is involved anywhere in this stack — the GC520 is a 2D-only
blit/fill/blend/stretch engine, and Weston runs its `pixman` (software)
renderer; every surface that matters is instead promoted straight onto a
hardware KMS plane (video, UI, cursor) and composited by the display
controller, not the CPU or a GPU shader.

---

## Repository layout

The six top-level areas below are listed in implementation order, bottom-up
— each one builds on the artifact(s) produced by the one above it.

```
drm-2d-gc520/
├── hal/           GC520 2D HAL: raw DE validation -> libetna2d -> KMS scanout
├── qpa/           Qt5 QPA platform plugin (etnaviv2d) on top of libetna2d
├── compositor/    Single-process dual-plane compositing (QPA UI + video, atomic KMS)
├── wayland/       Two-client Weston/Wayland compositor (QPA UI client + GStreamer video client)
├── ivi/           ivi-shell layer management (nu-hmi-controller Weston plugin)
└── patches/       Upstream patches (Weston, qtwayland, gst1-plugins-bad) + weston.ini configs
```

---

## Components and build artifacts

This is the minimum bill of materials for a working system. Installing each
artifact to the target (library paths, plugin directories, module
directories, config locations) is handled by the OS build system (Buildroot,
Yocto, or otherwise) via its own package/recipe — not covered here. What
follows is what each layer builds, and how to exercise it once installed.

### 1. HAL — `libetna2d`

The one library every layer above depends on: an opaque C API wrapping the
GC520 Drawing Engine's bitblt/stretchblt/alpha-blend commands.

Build: `hal/libetna2d` → shared library `libetna2d.so.1`.

Harness (acceptance test, no Qt/Weston involved):

```sh
./etna2d_selftest /dev/dri/renderD128
```

### 2. QPA — `libqetnaviv2d`

Qt5 platform plugin; accelerates `QPainter` through the GC520 DE via
`libetna2d`. Also the only place the Wayland-client transport
(`linux-dmabuf-v1`, `xdg_toplevel`/`ivi_surface`, `wl_seat`, hardware cursor)
lives — `compositor/` and `wayland/` reuse this same plugin unchanged.

Build: `qpa/` → Qt platform plugin `libqetnaviv2d.so`.

Harness (first QWidget rendered through the plugin, direct KMS, no Weston):

```sh
QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_DEBUG=1 ./test_integration
```

### 3. Weston patches

Weston 10.0.5 needs the four patches in `patches/weston/` applied at build
time. They make the DRM backend's `pixman`/no-GL path, the hardware cursor
plane, and `linux-dmabuf-v1` format advertisement work correctly. These are
plain `git am`/`patch -p1` patches against upstream Weston 10.0.5 source —
not tied to Buildroot, applicable the same way under any build system's
patch mechanism.

Two runtime configs (plain Weston `weston.ini`, not patches) select the
shell mode: `patches/weston/weston-kiosk.ini` (kiosk-shell) and
`patches/weston/weston-ivi.ini` (ivi-shell).

### 4. ivi-shell layer manager — `nu-hmi-controller`

Only needed when running ivi-shell. Assigns launch-order-independent layer
IDs/z-order to ivi-shell surfaces at runtime.

Build: `ivi/nu-hmi-controller/` → Weston module `nu-hmi-controller.so`,
loaded via `weston-ivi.ini`'s `modules=` line. No separate harness — it's
exercised as part of the two-client Wayland run below.

### 5. qtwayland patch

`patches/qt5wayland/0001-*.patch` — one-line `-DQT_EGL_NO_X11` fix for the
`qtwayland` module's compositor-side wayland-egl hardware integration, so it
builds on an X11-less embedded sysroot. Portable `git am` patch against the
`qtwayland` module source, not tied to any particular build system.

### 6. gst1-plugins-bad patch — GStreamer `waylandsink`

`patches/gst1-plugins-bad/0002-waylandsink-ivi-application-support.patch` —
adds an `ivi_surface` role to `waylandsink` alongside the existing
`xdg_toplevel` path, so a stock `gst-launch-1.0`/`waylandsink` pipeline can
present under ivi-shell as well as kiosk-shell. Applies against
gst1-plugins-bad (GStreamer ~1.22) the same way regardless of build system.

### 7. Two-client Wayland demo (Qt UI + video), for end-to-end verification

Once 1–6 above are in place, `wayland/` provides the reference two-client
setup: the QPA plugin as a Wayland client for the UI, and a video-side
client (`test_weston`, from `compositor/`) for NV12 content, both
hardware-plane-promoted under Weston.

Kick off (Weston already running with kiosk-shell or ivi-shell config):

```sh
# UI client
WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=etnaviv2d:plane=wayland ./qt_ui_client

# video client (kiosk-shell)
./test_weston --raw-video=./earth_nv12_1024x600.raw --video-size=1024x600

# video client (ivi-shell)
./test_weston --raw-video=./earth_nv12_1024x600.raw --video-size=1024x600 --ivi-id=1000
```

`verify_planes.sh` (`wayland/tests/`) cross-checks that both clients landed
on separate hardware planes via `/sys/kernel/debug/dri/0/state`.

### Test-only components (nothing to deploy)

A few programs exist purely to exercise a code path during development and
have no place on a deployed target — most notably `gst_plugin_dmabuf_src` (a
`dmabufsrc` GStreamer element used only to drive the zero-copy dmabuf path
from a local `gst-launch-1.0` pipeline before `waylandsink` support existed)
and `gst_dmabuf_video`, a standalone stand-in for a real hardware VPU
decoder, not a shipped component.

Similarly, `hal/kms/`'s benchmark binaries (`bench2d`, `bench_stretch`,
`kms_test`, `kms_gpu_test`) and the other `test_*` harnesses under `qpa/`
and `compositor/` are development/verification tools, not product
deliverables — they're mentioned above only where they double as the
acceptance check for a deployed layer.

---

## Config reference

Everything below is the minimum config needed to reproduce this stack —
verified hardware-side for Buildroot; the Yocto column is a first-pass
mapping (recipe/`local.conf` names may need adjustment) pending an actual
build/verification pass.

### Kernel config

| Option | Value | Why |
|---|---|---|
| `CONFIG_CMA` | `y` | Dumb-buffer (`CREATE_DUMB`) allocations backing every client BO ring are CMA-backed on this platform. |
| `CONFIG_DMA_CMA` | `y` | Required alongside `CONFIG_CMA` for the DRM/KMS dumb-buffer allocation path. |
| `CONFIG_CMA_SIZE_MBYTES` | `48` | **Not** the Buildroot/vendor-defconfig default (`16`). 16 MB is enough for exactly one client's buffer ring plus Weston's own shadow FB; a second simultaneous client (UI + video) then fails its first `CREATE_DUMB` with `ENOMEM`, since CMA also needs physically-contiguous space (fragmentation eats into the nominal free total). 48 MB matches the prior known-working tree (`ma35d1_br24_qt5`) and covers Weston's shadow FB + a 3-deep NV12 ring (~2.6 MB) + a 3-deep ARGB8888 ring (~7 MB) with headroom. |

No other kernel-side config is needed beyond the vendor's baseline MA35D1
defconfig (atomic-KMS-capable `nuvoton` DRM driver, etnaviv GPU node, etc.)
— this table only covers what we had to change from that baseline.

### Buildroot

| Option | Value | Why |
|---|---|---|
| `BR2_PACKAGE_LIBDRM` | `y` | Base libdrm. |
| `BR2_PACKAGE_LIBDRM_ETNAVIV` | `y` | `libdrm_etnaviv` — used directly by `libetna2d`. |
| `BR2_PACKAGE_LIBDRM_ENABLE_ATOMIC` | `y` | Atomic KMS API, required by Weston's `drm-backend.so`. |
| `BR2_PACKAGE_MESA3D` | `y` | Only pulled in as a **GBM provider** (`gbm_create_device()`/`gbm_bo_import()`) so Weston's pixman-renderer DRM backend can import client dmabufs for direct KMS scanout — no GL rendering happens through it anywhere in this stack. |
| `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SWRAST` | `y` | Deliberately the **software** Gallium driver, not `ETNAVIV` — the real GC520 GPU is driven directly by `libetna2d`/`libdrm_etnaviv`; a real Gallium/DRI Etnaviv driver would needlessly duplicate/contend for the same hardware. |
| `BR2_PACKAGE_MESA3D_OPENGL_EGL` | `y` | Auto-selects `BR2_PACKAGE_MESA3D_GBM`, which is the actual thing we need — Mesa's Kconfig doesn't expose GBM standalone. |
| `BR2_PACKAGE_MESA3D_OPENGL_ES` | `y` | Pulled in alongside EGL; not otherwise used by this stack. |
| `BR2_PACKAGE_MESA3D_GBM` | `y` (auto) | Provides `gbm.pc`/`gbm.h`/`libgbm.so` — without this, Weston silently compiles out its whole `#ifdef BUILD_DRM_GBM` dmabuf-scanout-under-pixman path with no build error, and every client surface fails to render. |
| `BR2_PACKAGE_WAYLAND` | `y` | Core Wayland library. |
| `BR2_PACKAGE_WAYLAND_PROTOCOLS` | `y` | Protocol XML for `wayland-scanner` (not needed at *this* project's build time since bindings are pre-generated, but needed by Weston's own build). |
| `BR2_PACKAGE_WESTON` | `y` | Weston 10.0.5. |
| `BR2_PACKAGE_WESTON_DEFAULT_DRM` / `BR2_PACKAGE_WESTON_DRM` | `y` | DRM/KMS backend. |
| `BR2_PACKAGE_WESTON_SHELL_KIOSK` | `y` | Used by all single/dual-client tests in this README; `weston.sh` launches Weston with `--shell=kiosk-shell.so` directly (no separate `weston-kiosk.ini` file). |
| `BR2_PACKAGE_WESTON_SHELL_IVI` | `y` | Optional — only needed for the ivi-shell + `nu-hmi-controller` mode. |
| `BR2_PACKAGE_LIBETNA2D` | `y` | This project's HAL package. `SITE_METHOD=git`, fetched from `MA35D1_Graphics` (`drm/libetna2d`), pinned to tag `v1.0`. |
| `BR2_PACKAGE_NU_HMI_CONTROLLER` | `y` | This project's ivi-shell layer manager. Same git fetch (`weston/ivi/nu-hmi-controller`). Optional, ivi-shell mode only. Relies on its Makefile's `WESTON_MAJOR?=10` default — no override needed since this BSP's Weston is 10.0.5. |
| `BR2_PACKAGE_QT5` / `BR2_PACKAGE_QT5BASE` | `y` | |
| `BR2_PACKAGE_QT5BASE_GUI` | `y` | |
| `BR2_PACKAGE_QT5BASE_WIDGETS` | `y` | Needed by `qt5-etnaviv2d-qpa`'s plugin build and its `etnaviv2d_benchmark` harness (`QT += widgets`). |
| `BR2_PACKAGE_QT5_ETNAVIV2D_QPA` | `y` | This project's QPA plugin package. Same git fetch (`qt/etnaviv2d-qpa`). Also builds and installs `etnaviv2d_benchmark` to `/usr/bin/` as part of this one package (no separate recipe). |
| `BR2_PACKAGE_QT5WAYLAND` | *(not set)* | Deliberately **not** needed — the QPA plugin's Wayland transport talks raw `libwayland-client` directly, not through QtWayland. |
| `BR2_PACKAGE_DEJAVU` / `_DEJAVU_MONO` / `_DEJAVU_SANS` | `y` | QtGui font-rendering fallback (fontconfig-discoverable path) — `qt5-etnaviv2d-qpa` no longer vendors its own `DejaVuSans.ttf` copy, this covers the same need at the distro level instead (matching the Yocto image's `ttf-dejavu-*` packages). |
| `BR2_PACKAGE_FONTCONFIG` / `BR2_PACKAGE_QT5BASE_FONTCONFIG` | `y` | Required for the above to actually be discoverable by Qt. |
| `BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV` | `y` | Weston `depends on BR2_PACKAGE_HAS_UDEV`; with plain `DYNAMIC_DEVTMPFS` Weston is hidden in menuconfig. Selecting this makes `BR2_PACKAGE_EUDEV` the udev provider. |
| `BR2_PACKAGE_EUDEV` (`BR2_PACKAGE_PROVIDES_UDEV="eudev"`) | `y` (auto) | udev provider for Weston, libinput and `libgudev`. |
| `BR2_PACKAGE_EUDEV_ENABLE_HWDB` | `y` | hwdb for libinput device classification (touch/pointer). |
| `BR2_PACKAGE_GSTREAMER1` | `y` | GStreamer core; hosts the video client pipeline (`gst-launch-1.0`). |
| `BR2_PACKAGE_GSTREAMER1_INSTALL_TOOLS` | `y` | Installs `gst-launch-1.0` / `gst-inspect-1.0`. |
| `BR2_PACKAGE_GST1_PLUGINS_BASE` | `y` | Base plugin set. |
| `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_PLAYBACK` / `_TYPEFIND` / `_VIDEOCONVERTSCALE` / `_VIDEOTESTSRC` / `_RAWPARSE` | `y` | `decodebin`/`playbin`, format detection, NV12 convert/scale, test source and raw NV12 input for the video client. |
| `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_AUDIOCONVERT` / `_AUDIORESAMPLE` / `_VOLUME` | `y` | Audio path for clips with sound. |
| `BR2_PACKAGE_GST1_PLUGINS_GOOD` + `_PLUGIN_ISOMP4` / `_AVI` / `_WAVPARSE` | `y` | Demuxers for the H.264 clips. |
| `BR2_PACKAGE_GST1_PLUGINS_BAD` | `y` | Hosts `waylandsink`, `v4l2codecs` and `kms`; patches `0002`/`0003` under `package/gstreamer1/gst1-plugins-bad/` apply automatically at build time. |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_WAYLAND` | `y` | `waylandsink` (dmabuf zero-copy to Weston, `ivi_surface` role via patch `0002`). |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_V4L2CODECS` | `y` | Stateless V4L2 H.264 decode on the Hantro VPU (auto-selects `libgudev`). |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_VIDEOPARSERS` | `y` | `h264parse`, required in front of the stateless decoder. |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_KMS` | `y` | `kmssink`, direct-KMS video output when Weston is not running. |

Plus the 4 local Weston patches under `package/weston/0002`–`0005-*.patch`
(same content as `patches/weston/10/` here) applied at Weston's build time —
not optional, see §"Weston patches" above for what each one does.

### Yocto

| Recipe / `local.conf` variable | Value | Buildroot equivalent |
|---|---|---|
| Kernel `defconfig`: `CONFIG_CMA_SIZE_MBYTES=48` | `48` | same kernel option, same shared kernel source defconfig as Buildroot (see Kernel config note above) |
| `nvt-ma35d1-weston.conf`: `IMAGE_INSTALL:append += " libdrm libdrm-tests mesa libetna2d qt5-etnaviv2d-qpa nu-hmi-controller gstreamer1.0-plugins-bad weston weston-init weston-examples"` | | `BR2_PACKAGE_LIBDRM` / `_ETNAVIV`, `_MESA3D` + `_GBM`, the 3 custom packages, `_WESTON` |
| `nvt-ma35d1-weston.conf`: `DISTRO_FEATURES:append = " wayland opengl pam"` | | `opengl` here only pulls in Mesa's GBM/EGL (software rasterizer) for Weston's dmabuf-scanout-under-pixman path, same role as Buildroot's `MESA3D_GALLIUM_DRIVER_SWRAST` — no GPU rendering goes through it |
| `nvt-ma35d1-weston.conf`: `PACKAGECONFIG:remove:pn-qtbase = "directfb"`, `PACKAGECONFIG:append:pn-qtbase = " kms"` | | `qtbase`'s shared bbappend enables `directfb` unconditionally (shared with `nvt-ma35d1-directfb`); the Weston distro removes it and adds `kms` instead, needed for `qt5-etnaviv2d-qpa` to find `libdrm`'s headers via Qt's `kms_support` module |
| `libetna2d_1.0.bb` / `nu-hmi-controller_1.0.bb` / `qt5-etnaviv2d-qpa_1.0.bb` | `SRC_URI = git://github.com/OpenNuvoton/MA35D1_Graphics.git;branch=master`, `SRCREV` pinned to the commit behind tag `v1.0` | this project's 3 custom Buildroot packages — same `MA35D1_Graphics` source/subdirs, Yocto recipes use `do_install()`/`do_compile:append` (qt5-etnaviv2d-qpa only, for `etnaviv2d_benchmark`) instead of Buildroot's `*_BUILD_CMDS`/`*_POST_BUILD_HOOKS` |
| `nu-hmi-controller_1.0.bb`: `WESTON_MAJOR=13` passed via `EXTRA_OEMAKE` | | Buildroot relies on the Makefile's `WESTON_MAJOR?=10` default instead (Weston 10.0.5 there vs 13.0.1 here) |
| `weston_13.0.1.bbappend`: 3 patches under `patches/weston/13/` | | `patches/weston/10/`'s 4 patches, §"Weston patches" — none dropped, but Buildroot's `0002` (linux-dmabuf advertisement) and `0004` (KMS-plane-format advertisement) are folded into one `0002` patch here, since their `init_pixman()` hook registration is now interleaved in 13.0.1 |
| `weston-init.bbappend`: `PACKAGECONFIG:append = " use-pixman"`, `DEFAULTBACKEND = "drm"`, ships `weston-ivi.ini` alongside the default kiosk `weston.ini`. `weston-ivi.ini` itself also needs its own `use-pixman=true` under `[core]` (`weston-init`'s `PACKAGECONFIG[use-pixman]` only injects it into the default `weston.ini`, not the separately-shipped ivi one) — omitting it silently falls back to the GL/EGL renderer path, which this BSP doesn't have, causing CMA exhaustion/IO-wait thrashing under ivi-shell | | `BR2_PACKAGE_WESTON_SHELL_KIOSK` + `_SHELL_IVI`, `patches/weston/weston-ivi.ini` |
| `qtwayland_git.bbappend`: `0001-compositor-wayland-egl-define-QT_EGL_NO_X11-for-no-X.patch` | | `BR2_PACKAGE_QT5WAYLAND` deliberately unset on Buildroot — `qtwayland` is built on Yocto only as a not-currently-used dependency, kept patched/buildable for a possible future QtQuick compositor |
| `qt5-env.bbappend`: `do_install:append` sed-patches `QT_QPA_PLATFORM=etnaviv2d` and drops the unconditional `XDG_RUNTIME_DIR` export (`nvt-ma35d1-weston` only) | | n/a — Buildroot's `qt5-env.sh` equivalent needed no such override |
| `nvt-image-qt5.bb`: `ttf-dejavu-sans(-mono/-condensed)`, `ttf-dejavu-serif(-condensed)` in `IMAGE_INSTALL`; `SYSTEMD_DEFAULT_TARGET:nvt-ma35d1-weston = "graphical.target"`; `kernel-module-etnaviv` installed as a hard `IMAGE_INSTALL:append` dependency when `DISTRO == nvt-ma35d1-weston` (previously a soft `PACKAGE_INSTALL_ATTEMPTONLY`, which silently shipped a GPU-less image if the module was missing — now fails the build loudly instead) | | `BR2_PACKAGE_DEJAVU` + `_MONO` + `_SANS`, `BR2_PACKAGE_FONTCONFIG` |
| `gstreamer1.0-plugins-bad_1.22.12.bbappend`: `0002-waylandsink-ivi-application-support.patch` (applied only when `DISTRO_FEATURES` contains `wayland`) | | same patch, `patches/gst1-plugins-bad/`, applied unconditionally there (only one Buildroot distro in this stack) |
| `gc520_1.7.7.bb` / `vc8000_0.9.0.bb`: `COMPATIBLE_MACHINE` disabled when `DISTRO == nvt-ma35d1-weston` | | these two DirectFB-era packages aren't part of this stack at all on Buildroot either — not installed by this project's `.config` |

All of the above is confirmed building and running on hardware for both
`nvt-ma35d1-weston` (Yocto) and this project's Buildroot `.config`.

