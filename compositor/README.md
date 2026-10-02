# Plane-aware Compositor — Build & Test Guide

Multi-plane scanout for the etnaviv2d QPA on Nuvoton MA35D1: a single Qt
process driving GUI (overlay plane, blended) and video (primary plane,
opaque) through one atomic KMS commit, plus a Weston hardware-plane-promotion
gate for the future Wayland-compositor path.

See `plan.md` for the full design, decision log (D1–D22), and risk closures.
This guide only covers building, deploying, and running the tests.

> **Note:** the `qpa/` plugin has been extended since `qpa/README.md` was
> written (Wayland transport with `linux-dmabuf-v1`, `xdg_toplevel`/
> `ivi_surface` dual-mode shell support, `wl_seat` input, hardware cursor).
> `qpa/README.md`'s build steps still apply unchanged — `make plugin` picks up
> the new sources automatically — so they are not repeated here.

---

## Repository layout

```
compositor/
├── README.md      this file
├── plan.md         design decisions, architecture, decision log
├── Makefile        top-level build orchestrator
├── images/         test assets (PNG + raw NV12) and generation notes
├── build/          build output + log.md (raw hardware run transcripts)
└── tests/
    ├── test_alpha_blend/  Step 3 — overlay blend correctness (two-instance, shared fd)
    ├── test_composite/    Step 4 — single-committer dual-plane (video + UI)
    └── test_weston/       Step 6 — Weston linux-dmabuf-v1 hardware-plane-promotion gate
```

---

## Prerequisites

Same Buildroot workspace as `hal/` and `qpa/`:

```sh
source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup
```

- `test_alpha_blend` / `test_composite` — Qt apps, built with the in-tree
  qmake (same reasoning as `qpa/README.md` — private Qt headers aren't needed
  here, but the Makefile mirrors the qpa pattern for consistency).
- `test_weston` — a bare-C Wayland client (no Qt). Needs `wayland-client` and
  `libdrm` resolvable via `pkg-config`, which the sourced environment-setup
  provides.
- The `qpa/etnaviv2d` plugin and `libetna2d` must already be built and
  deployed to the target (see `qpa/README.md`) — `test_alpha_blend` and
  `test_composite` load the plugin at runtime via `QT_QPA_PLATFORM=etnaviv2d`.
- For Step 6 (`test_weston`), the target rootfs needs Weston with the DRM
  backend and kiosk shell (`BR2_PACKAGE_WESTON`, `BR2_PACKAGE_WESTON_DRM`,
  `BR2_PACKAGE_WESTON_SHELL_KIOSK`).

---

## Build

```sh
cd compositor
make all               # all three tests
make test_alpha_blend  # individual target
make test_composite
make test_weston
make clean             # remove build/
```

Override the Qt build tree if it differs from the default:

```sh
make all QT_BUILD=/path/to/qt5base-build
```

Artifacts:

```
build/test_alpha_blend/test_alpha_blend
build/test_composite/test_composite
build/test_weston/test_weston
```

---

## Deploy to target

```sh
BOARD=root@192.168.1.100
CMP=/home/joeylu/opencode/graphic/drm-2d-gc520/compositor

scp $CMP/build/test_alpha_blend/test_alpha_blend $BOARD:/tmp/
scp $CMP/build/test_composite/test_composite     $BOARD:/tmp/
scp $CMP/build/test_weston/test_weston           $BOARD:/tmp/

# Test assets
scp -r $CMP/images $BOARD:/tmp/
```

`images/readme.md` documents how `earth_nv12_1024x600.raw` was generated from
`earth.mp4` via `ffmpeg`, and how to preview it on the host with `ffplay`.

---

## Tests

Each test validates one layer of the plane-aware compositor independently, in
the same order the plan built them (Steps 3, 4, 6). Run them in order the
first time so a failure is easy to localise.

### Step 3 — `test_alpha_blend` (overlay blend correctness)

**Purpose:** proves the overlay plane blends `SRC_OVER` correctly
(premultiplied alpha, no colour conversion) over the primary plane, using two
`QEtnaviv2dScreen` instances sharing one DRM master fd — the two-instance
model retained as the blend-correctness regression (see plan.md D13/D15).

```sh
# On target:
QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=primary \
    ./test_alpha_blend [--image=./images/earth.png] [--frames=N] [--verbose]
```

- `--image=PATH` — background PNG shown on the primary plane (default
  `./images/earth.png`).
- `--frames=N` — stop after N overlay frames (default 0 = run forever).
- `--verbose` — print the expected patch values every 30 frames.

What to look for on the panel: four stacked patches over the background
image — transparent (α=0, background shows through), 50% red (α=128,
premultiplied), opaque white (α=255, background fully blocked), and an
animated semi-transparent cyan HUD box with a live FPS counter. No halo or
dark/bright fringe at patch edges indicates the blend factors are correct.

### Step 4 — `test_composite` (single-commit video + UI)

**Purpose:** proves the single-committer, single-vblank design (plan.md
D3/D9/D10/D12) — one `QEtnaviv2dScreen`, in `plane=composite` mode, folds a
video frame on the primary plane (NV12, opaque) and a Qt UI on the overlay
plane (ARGB, `SRC_OVER`) into **one** atomic commit. Exercises the 3-buffer
NV12 ring (D9b), the video-as-present-trigger path (D12) via
`--static-after`, and tear-free playback.

```sh
# On target:
QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=composite \
    ./test_composite \
        --raw-video=./images/earth_nv12_1024x600.raw \
        --video-size=1024x600 \
        [--video-fps=30] [--static-after=N] [--frames=N] [--verbose]
```

- `--raw-video=FILE` — mandatory. Pre-decoded flat NV12 file, looped.
- `--video-size=WxH` — frame dimensions (default 1024x600).
- `--video-fps=N` — playback rate (default 30).
- `--static-after=N` — freeze UI animation after N frames, to verify the
  video keeps advancing on its own even with no UI repaints (D12).
- `--frames=N` — quit after N video frames (default 0 = loop forever).
- `--verbose` — extra logging.

What to look for: the same four reference patches as `test_alpha_blend`, now
composited over moving video instead of a static image, plus an animated
bouncing HUD box. Confirm: no tearing on the video edge (single commit ⇒
single vblank), and after `--static-after=N` fires, the video keeps playing
while the HUD freezes in place.

### Step 6 — `test_weston` (Weston hardware-plane-promotion gate)

**Purpose:** validates the Phase-3 "Weston owns plane arbitration" model —
proves a Wayland client's fullscreen NV12 **dmabuf** surface is promoted by
Weston's DRM backend to a real hardware plane (not GL-composited), tear-free,
at source frame rate. This is a bare-C client (no Qt/QPA) that mirrors
`test_composite`'s 3-buffer NV12 discipline over `linux-dmabuf-v1` instead of
direct KMS, de-risking the future QPA Wayland transport.

Supports both `xdg_toplevel` (desktop-style fullscreen) and `ivi_surface`
(`ivi-shell`, via `nu-hmi-controller`) shells — it auto-detects whichever
`ivi_application`/`xdg_wm_base` global Weston advertises.

Start Weston first (kiosk shell, DRM backend):

```sh
weston --tty=1 --backend=drm-backend.so --shell=kiosk-shell.so &
# or, for driver-level plane-assignment logging:
weston --backend=drm --shell=kiosk --logger-scopes=drm-backend &
```

Then run the client:

```sh
WAYLAND_DISPLAY=wayland-1 ./test_weston \
    --raw-video=./images/earth_nv12_1024x600.raw \
    --video-size=1024x600 \
    [--video-fps=30] [--frames=N] [--ivi-id=N] [--verbose]
```

- `--raw-video=FILE` — mandatory. Same pre-decoded NV12 file as
  `test_composite`.
- `--video-size=WxH` — frame dimensions (default 1024x600).
- `--video-fps=N` — playback rate (default 30).
- `--frames=N` — stop after N frames (default 0 = loop forever).
- `--ivi-id=N` — IVI surface ID when Weston runs `nu-hmi-controller`
  (default 1000); selects which layer/plane the surface lands on,
  independent of launch order.
- `--verbose` — verbose logging.

What to confirm:
- Weston's `drm-backend` log (or `--logger-scopes=drm-backend`) shows the
  client's surface assigned to a hardware plane, not composited by Pixman/GL.
- On target, `/sys/kernel/debug/dri/0/state` shows the overlay plane active
  with `format=NV12` and the expected `crtc` while the client runs.
- Tear-free playback at the source frame rate on the panel.

---

## Environment variables

| Variable | Effect |
|---|---|
| `QT_QPA_PLATFORM=etnaviv2d` | Select the etnaviv2d QPA plugin (`test_alpha_blend`, `test_composite`) |
| `QT_ETNAVIV2D_PLANE=<mode>` | Plane mode: `primary`, `overlay`, `composite`, `wayland`/`auto` — see `qpa/README.md` and `plan.md` §"QPA argument" |
| `QT_ETNAVIV2D_DEBUG=1` | Verbose plugin logging (see `qpa/README.md`) |
| `WAYLAND_DISPLAY=wayland-1` | Wayland socket for `test_weston` (and for the QPA's `wayland`/`auto` plane mode once Weston is running) |

