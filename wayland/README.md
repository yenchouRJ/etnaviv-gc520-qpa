# Two-client Wayland Compositor Guide

Runs the etnaviv2d QPA as a Weston Wayland client (Qt UI, `linux-dmabuf-v1`,
zero-copy) alongside a second Weston client presenting NV12 video, both
hardware-plane-promoted, with Weston owning plane arbitration and input.

See `plan.md` for the full design, the Weston patch rationale, and the
decision log. This guide only covers building, deploying, and running the
tests.

> **Note:** this reuses and extends the `qpa/` plugin (new
> `QEtnaviv2dWaylandTransport` filling in the `plane=wayland` handoff stub
> with a real client: `linux-dmabuf-v1` ARGB ring, `xdg_toplevel`/
> `ivi_surface`, `wl_seat` input, hardware cursor). `qpa/README.md`'s build
> steps still apply unchanged — `make plugin` picks up the new transport
> sources automatically — so they are not repeated here.
>
> **Note:** `compositor/tests/test_weston` (the video-side Wayland client
> reused below) has also gained dual-mode shell support since
> `compositor/README.md` was written — it now auto-detects `ivi_application`
> and creates an `ivi_surface` (with `--ivi-id=N`) instead of an
> `xdg_toplevel` when Weston runs ivi-shell. See §"IVI-shell mode" below.

---

## Repository layout

```
wayland/
├── README.md      this file
├── plan.md         design, Weston patches, decision log
├── model.md        supporting notes
├── Makefile        build orchestrator for the test harnesses below
├── images/         shared NV12 .raw test asset
├── build/          build output + log.md (raw hardware run transcripts)
└── tests/
    ├── qt_ui_client/          minimal Qt app exercising the QPA Wayland transport (UI client)
    ├── gst_dmabuf_video/      standalone C program: DRM dumb-BO ring → appsrc → waylandsink (video client)
    ├── gst_plugin_dmabuf_src/ GStreamer dmabufsrc plugin (.so) for plain gst-launch-1.0 pipelines
    └── verify_planes.sh       dri/0/state dual hardware-plane-promotion cross-check
```

The video side also reuses `compositor/tests/test_weston` (built and
deployed from `compositor/`, see `compositor/README.md`). The optional
ivi-shell HMI controller lives in the sibling `ivi/` directory:

```
ivi/
├── plan.md                design + status of the ivi-shell capability
└── nu-hmi-controller/      Weston HMI controller plugin (custom layer routing)
```

---

## Prerequisites

Same Buildroot workspace as `hal/`, `qpa/`, and `compositor/`:

```sh
source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup
```

- `qt_ui_client` — Qt app, built with the in-tree qmake.
- `gst_dmabuf_video` / `gst_plugin_dmabuf_src` — bare-C, need
  `gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-allocators-1.0` (plugin
  also needs `gstreamer-base-1.0`/`gstreamer-video-1.0`), and `libdrm`
  resolvable via `pkg-config` against the sysroot.
- The updated `qpa/etnaviv2d` plugin and `libetna2d` must already be built
  and deployed to the target (see `qpa/README.md`) — `qt_ui_client` loads the
  plugin via `QT_QPA_PLATFORM=etnaviv2d:plane=wayland`.
- `compositor/tests/test_weston` built and deployed (see
  `compositor/README.md`) — used here unmodified as the video-side client.
- Target rootfs: Weston with the DRM backend and kiosk shell, plus the
  Weston patches in `plan.md` §"Weston patches" (GBM+`linux-dmabuf` under
  pixman, PLANES_ONLY fixes, `get_scanout_formats_hook`,
  `DRM_FORMAT_MOD_INVALID` → `DRM_FORMAT_MOD_LINEAR`). Buildroot config:
  `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_WAYLAND` (`waylandsink`),
  `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_RAWPARSE` (`rawvideoparse`),
  `BR2_PACKAGE_QT5WAYLAND` (headers only, not used at runtime).

---

## Build

```sh
cd wayland
make all                     # all three test harnesses
make qt_ui_client            # individual target
make gst_dmabuf_video
make gst_plugin_dmabuf_src
make clean                   # remove build/
```

Override paths if your workspace differs:

```sh
make all QT_BUILD=/path/to/qt5base-build SYSROOT=/path/to/sysroot
```

Artifacts:

```
build/qt_ui_client/qt_ui_client
build/gst_dmabuf_video/gst_dmabuf_video
build/gst_plugin_dmabuf_src/libgstdmabufsrc.so
```

---

## Deploy to target

```sh
BOARD=root@192.168.1.100
WLD=/home/joeylu/opencode/graphic/drm-2d-gc520/wayland

scp $WLD/build/qt_ui_client/qt_ui_client                 $BOARD:/tmp/
scp $WLD/build/gst_dmabuf_video/gst_dmabuf_video          $BOARD:/tmp/
scp $WLD/build/gst_plugin_dmabuf_src/libgstdmabufsrc.so   $BOARD:/tmp/
scp $WLD/tests/verify_planes.sh                           $BOARD:/tmp/
scp -r $WLD/images                                        $BOARD:/tmp/

# Reused from compositor/ (video-side Wayland client)
scp /home/joeylu/opencode/graphic/drm-2d-gc520/compositor/build/test_weston/test_weston $BOARD:/tmp/
```

Weston itself, the kiosk config, and any custom shell plugins are deployed
per the target's Buildroot image / rootfs overlay (see `plan.md` §"Buildroot
config" and §"Weston patches").

---

## Start Weston

```sh
weston --tty=1 --backend=drm-backend.so --shell=kiosk-shell.so --use-pixman &
```

All tests below run as Weston clients against this instance
(`WAYLAND_DISPLAY=wayland-1` by default).

---

## Tests

### `qt_ui_client` — QPA Wayland transport (UI client)

**Purpose:** exercises `QEtnaviv2dWaylandTransport` end-to-end: connects to
Weston, allocates the 3-deep ARGB8888/LINEAR BO ring, exports each as a
persistent `wl_buffer` via `linux-dmabuf-v1`, creates an `xdg_toplevel` (or
`ivi_surface` if `ivi_application` is advertised), and drives frame pacing
via `wl_surface.frame` + `wl_buffer.release`. Also exercises `wl_seat` input
(touch/mouse/keyboard translated to Qt events) and the hardware cursor
plane.

```sh
WAYLAND_DISPLAY=wayland-1 \
QT_QPA_PLATFORM=etnaviv2d:plane=wayland \
QT_ETNAVIV2D_DEBUG=1 \
    ./qt_ui_client [--no-gradient]
```

- *(no args)* — fullscreen animated gradient background plus alpha-blend
  reference patches.
- `--no-gradient` — skip the opaque animated gradient; renders a large
  semi-transparent patch instead (useful to confirm plane eligibility is
  driven by view-level alpha, not per-pixel transparency).

A large button covers the lower half of the screen and increments a counter
on tap/click — confirms `wl_touch`/`wl_pointer` events are reaching Qt. Move
a mouse over the window to confirm the hardware cursor (arrow) appears and
tracks smoothly.

What to check:
- Terminal logs `Connected to Wayland display`, `xdg_surface configured`
  (or ivi-shell equivalent), `ARGB ring allocated: 3 x W x H
  ARGB8888/LINEAR`.
- `cat /sys/kernel/debug/dri/0/state` shows the UI surface's plane active
  with format `AR24`.

### `gst_dmabuf_video` — zero-copy NV12 video client (standalone)

**Purpose:** stands in for a real GStreamer VPU/`waylandsink` pipeline.
Allocates a 3-deep ring of DRM dumb BOs, reads `.raw` NV12 frames into them,
exports each as a DMABuf fd, and pushes `memory:DMABuf` NV12 buffers via
`appsrc` into an internal `waylandsink` pipeline at a fixed rate — proving
the zero-copy dmabuf → hardware-plane path without requiring a VPU decoder
or a GStreamer dmabuf-capable source element.

```sh
XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1 \
    ./gst_dmabuf_video \
        --video=./images/earth_nv12_1024x600.raw \
        --width=1024 --height=600 [--fps=30]
```

What to check: video plays smoothly (frame counter advancing, no ring
stall); `/sys/kernel/debug/dri/0/state` shows the video surface's plane
active with format `NV12`.

### `gst_plugin_dmabuf_src` — dmabufsrc GStreamer plugin

**Purpose:** the same DRM dumb-BO/DMABuf ring logic as `gst_dmabuf_video`,
packaged as a proper GStreamer source element (`dmabufsrc`) so the same
zero-copy path can be driven from a plain `gst-launch-1.0` pipeline instead
of a bespoke C program.

```sh
XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1 \
GST_PLUGIN_PATH=/tmp \
    gst-launch-1.0 dmabufsrc \
        location=./images/earth_nv12_1024x600.raw width=1024 height=600 \
        ! waylandsink sync=false
```

What to check: same as `gst_dmabuf_video` — NV12 on a hardware plane, smooth
playback.

### `test_weston` (reused from `compositor/`) — video client alternative

**Purpose:** in place of `gst_dmabuf_video`/`gst_plugin_dmabuf_src`, the
existing `compositor/tests/test_weston` binary (see `compositor/README.md`,
built/deployed from `compositor/`) can be run as the video-side Weston
client — it implements the `linux-dmabuf-v1` NV12 ring and auto-detects
`xdg_wm_base` (kiosk-shell) vs. `ivi_application` (ivi-shell), so the same
binary works under either shell with no rebuild.

```sh
./test_weston \
    --raw-video=./images/earth_nv12_1024x600.raw \
    --video-size=1024x600 [--video-fps=30] [--ivi-id=N]
```

`--ivi-id=N` (default 1000) only takes effect when Weston is running
ivi-shell — see §"IVI-shell mode" below.

### Two-client plane arbitration — `verify_planes.sh`

**Purpose:** with both a UI client (`qt_ui_client`) and a video client
(`test_weston` or `gst_dmabuf_video`) running simultaneously, confirms
Weston assigns **both** surfaces to real hardware planes (not
renderer-composited), drops its own shadow framebuffer, and places the UI
above the video by zpos.

```sh
# Start the video client first, then the UI client, e.g.:
./test_weston --raw-video=./images/earth_nv12_1024x600.raw \
    --video-size=1024x600 --video-fps=30 &
WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=etnaviv2d:plane=wayland \
    ./qt_ui_client --no-gradient &

# Then, once both are up and rendering:
mount -t debugfs none /sys/kernel/debug   # if not already mounted
sh ./verify_planes.sh
```

Checks performed (exit 0 = all pass):
1. NV12 (video) is on a hardware plane.
2. ARGB/AR24 (UI) is on a hardware plane.
3. Weston's own shadow framebuffer (XR24) is absent — confirms both client
   surfaces fully cover the output and there is zero compositor overhead.
4. At least two planes are active simultaneously.

Reads `/sys/kernel/debug/dri/0/state` directly; can also be inspected
manually for a single-client run.

---

---

## IVI-shell mode (alternate shell, deferred as a full product feature)

Weston can run with **kiosk-shell** (used everywhere above — plane
assignment follows Weston's implicit view-stacking order) or with
**ivi-shell** plus a custom HMI controller, `ivi/nu-hmi-controller`, which
gives each client **explicit, launch-order-independent** control over which
plane/layer it lands on via an application-assigned IVI surface ID. Both
`qt_ui_client` and `test_weston` auto-detect whichever shell Weston is
running (no rebuild needed) — see `ivi/plan.md` for the full design and
current status. This capability is implemented and hardware-verified, but
not yet a shipped deliverable: GStreamer's `waylandsink` cannot bind an IVI
surface ID (it always creates a plain `xdg_toplevel`), so
`gst_dmabuf_video`/`gst_plugin_dmabuf_src` fall through to a catch-all
overflow layer under ivi-shell rather than being ID-routed.

### Build and deploy `nu-hmi-controller`

```sh
cd ivi/nu-hmi-controller
source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup
make
# -> build/nu-hmi-controller.so

scp build/nu-hmi-controller.so root@192.168.1.100:/usr/lib/weston/
```

### `weston.ini` — select ivi-shell and declare layers

```ini
[core]
shell=ivi-shell.so
modules=nu-hmi-controller.so

[nuvoton-layer]
id=1000
render-order=0

[nuvoton-layer]
id=2000
render-order=1
```

Each `[nuvoton-layer]` section creates one ivi-shell layer at startup; a
client's `ivi_surface` is routed to the layer whose `id` matches the
surface's IVI ID. Surfaces with no matching layer (or created via
`xdg_toplevel` under ivi-shell) fall through to a fixed catch-all overflow
layer.

### Switching Weston between kiosk-shell and ivi-shell

```sh
# kiosk-shell (implicit, stacking-order plane assignment)
weston --tty=1 --backend=drm-backend.so --shell=kiosk-shell.so --use-pixman &

# ivi-shell (explicit, ID-driven plane assignment) — needs a weston.ini
# with the [core]/[nuvoton-layer] block above, e.g. via --config=
weston --tty=1 --backend=drm-backend.so \
    --config=/etc/xdg/weston/weston-ivi.ini \
    --shell=ivi-shell.so --use-pixman &
```

Only one Weston instance can own `/dev/dri/card0` at a time — stop the
current one (`killall weston`) before starting the other.

### Running the clients under ivi-shell

```sh
# Video on layer 1000
./test_weston --raw-video=./images/earth_nv12_1024x600.raw \
    --video-size=1024x600 --video-fps=30 --ivi-id=1000 &

# Qt UI on layer 2000 (higher render-order → on top)
QT_IVI_SURFACE_ID=2000 WAYLAND_DISPLAY=wayland-1 \
QT_QPA_PLATFORM=etnaviv2d:plane=wayland \
    ./qt_ui_client --no-gradient &
```

Because placement is ID-driven rather than stacking-order-driven, the two
clients can be started in **either order** and still land on the correct
layers — swap the ID values (`--ivi-id=2000` for video,
`QT_IVI_SURFACE_ID=1000` for the UI) to verify z-order is controlled purely
by ID, not by launch order.

---

## Environment variables

| Variable | Effect |
|---|---|
| `WAYLAND_DISPLAY` | Wayland socket to connect to (set by Weston, typically `wayland-1`) |
| `QT_QPA_PLATFORM=etnaviv2d:plane=wayland` | Select the etnaviv2d QPA Wayland transport for `qt_ui_client` |
| `QT_ETNAVIV2D_DEBUG=1` | Verbose transport logging (connect, ring allocation, configure events) |
| `QT_IVI_SURFACE_ID=N` | Explicit `ivi_surface` ID for the QPA client when Weston runs ivi-shell (pairs with `test_weston --ivi-id=N`) |
| `XDG_RUNTIME_DIR` | Wayland/X-style runtime dir; must match Weston's (e.g. `/tmp/xdg`) for all clients |
| `GST_PLUGIN_PATH` | Directory containing `libgstdmabufsrc.so`, for the `gst-launch-1.0 dmabufsrc` pipeline |
