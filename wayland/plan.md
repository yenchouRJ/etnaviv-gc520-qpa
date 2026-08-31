# Phase 7 — Two-client Wayland composition (Qt UI + video) under Weston

**Goal:** Run the etnaviv2d QPA as a **Weston Wayland client** that presents the
Qt UI to a hardware plane via `linux-dmabuf-v1`, alongside a **second client**
(GStreamer `waylandsink`) presenting NV12 video to the other plane — both
zero-copy, both hardware-promoted, Weston owning plane arbitration. This is
**D11** from the Phase 6 plan: the QPA is *adapted, not replaced* — only its
transport switches from direct KMS to Wayland; `libetna2d` / `EtnaBlittable` /
the GC520 blitter are untouched.

> **Status (2026-08-28):** Milestones B, C, and A are hardware-verified and closed.
> Milestone I (`wl_seat` input) is next.

> **Prerequisite:** Phase 6 (`compositor/plan.md`) closed. `test_weston` proved
> the NV12 dmabuf → hardware plane path with a bare C client; Phase 7 reuses that
> exact transport from inside the QPA and adds a second client.

---

## The model

```
── Phase 7: two Weston clients, two hardware planes ─────────────

  Qt app (etnaviv2d QPA, plane=wayland)      GStreamer
     │  EtnaBlittable → ARGB etna2d_surface       │  filesrc ! rawvideoparse
     │  (GC520 2D blit, unchanged)                │    ! waylandsink
     │                                            │
     │  ARGB BO ring → dmabuf                     │  NV12 dmabuf
     │    → linux-dmabuf-v1                       │    → linux-dmabuf-v1
     │    → xdg_toplevel (fullscreen)             │    → wl_surface
     └──────────────┐                    ┌────────┘
                    ▼                    ▼
              Weston compositor (sole DRM master, sole input master)
                 │  drmModeAtomicCommit — assigns each client surface
                 │  to a hardware plane by zpos; PLANES_ONLY mode;
                 │  no shadow FB (both surfaces cover the output)
                 └──────── /dev/dri/card0
                                │
                    ┌───────────┴───────────┐
              plane[31] zpos=0: video    plane[38] zpos=1: Qt UI
              (primary, NV12)            (overlay, AR24)
```

**The render layer is invariant.** `EtnaBlittable` still renders the Qt scene
into an ARGB `etna2d_surface`; the blitter never learns about Wayland. Only the
*transport* changes — the `isStub()` seam in `QEtnaviv2dScreen`
(`qetnaviv2dscreen.h:157`, `m_planeMode == PlaneMode::Wayland`) is filled in by a
new sibling transport class.

| Phase | Render (invariant) | Transport |
|-------|--------------------|-----------|
| No compositor (P1–2) | `EtnaBlittable` → `etna2d_surface` | dmabuf → `drmModeAtomicCommit` (direct KMS) |
| Weston (P7, this plan) | `EtnaBlittable` → `etna2d_surface` | dmabuf → `linux-dmabuf-v1` + `xdg_toplevel` → `wl_surface_commit`; Weston picks the plane |

---

## Target hardware (confirmed)

MA35D1, single CRTC, three planes. **Decisive fact for Phase 7:** on MA35D1
**both the primary and overlay planes support the required RGB *and* YUV
formats** — so plane assignment is *not* constrained by NV12-vs-ARGB
capability. The only immutable hardware constraint is **fixed z-order (zpos)**:

| Plane   | zpos | Formats | Role in Phase 7 |
|---------|------|---------|-----------------|
| PRIMARY [31] | 0 (bottom) | RGB + YUV | video (NV12) |
| OVERLAY [38] | 1 (middle) | RGB + YUV | Qt UI (AR24) |
| CURSOR  [49] | 2 (top)    | XRGB8888 32×32 | unused |

Because the two planes are format-equivalent, the **key validation point is
correct zpos-based plane allocation, not plane format capability**. The visual
target is **UI on the higher-zpos plane (overlay, blended) over video on the
lower-zpos plane (primary, opaque)** (§Decision D7-zpos).

---

## Weston patches

> **Note:** Weston patch files in `package/weston/` will be regenerated with
> `git format-patch` at the end of the dev journey, once all fixes are stable.
> Until then, the working state lives in the weston git tree at
> `output/build/weston-10.0.5`; the table below documents every change applied.

| Area | File(s) changed | What |
|---|---|---|
| 0001 (pre-existing) | fbdev backend | fbdev build fix |
| 0002 | `drm.c`, `pixman-renderer.c`, `linux-dmabuf.c`, `meson.build` | GBM init in `init_pixman()`; linux-dmabuf advertised without GL renderer; pixman-renderer accepts dmabuf buffers; `BUILD_DRM_GBM` enabled |
| 0003 | `state-propose.c` | **Fix 1:** skip no-buffer views (kiosk background) in PLANES_ONLY. **Fix 2:** allow topmost fullscreen view to try overlay when ≥2 client views exist |
| 0003 (cont.) | `drm.c`, `linux-dmabuf.c`, `include/libweston/libweston.h` | `get_scanout_formats_hook` on `weston_compositor`; set in `init_pixman()` to return KMS plane format union so `zwp_linux_dmabuf_v1` advertises NV12/YUV formats in pixman mode |
| 0003 (cont.) | `fb.c` | `DRM_FORMAT_MOD_INVALID` → `DRM_FORMAT_MOD_LINEAR` for dumb BO imports; allows `gst_dmabuf_allocator` buffers (no modifier metadata in GStreamer ≤ 1.22) to reach KMS plane |

---

## Buildroot config (prepared — for reference)

All four symbols are set; documented here so a later dev can reproduce the image.

| Config symbol | Why |
|---|---|
| `BR2_PACKAGE_GST1_PLUGINS_BAD=y` | Parent package for `waylandsink` |
| `BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_WAYLAND=y` | `waylandsink` — GStreamer Wayland video output client |
| `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_RAWPARSE=y` | `rawvideoparse` — feed the `.raw` NV12 file into the pipeline without a VPU decoder |
| `BR2_PACKAGE_QT5WAYLAND=y` | Qt Wayland QPA + client headers; required for a Qt app to connect to Weston as a client |

Already satisfied by the Phase 6 / Mesa config (no change needed):
`wayland`, `wayland-protocols`, `libdrm`, `libegl-wayland`, `libgles`,
`BR2_PACKAGE_MESA3D_GBM`, `BR2_PACKAGE_WESTON_DRM`,
`BR2_PACKAGE_WESTON_SHELL_KIOSK`, and the Weston source patches
(`package/weston/0002-*.patch`, `-Drenderer-gl=true`).

> Note: `videoconvert`/`videoscale` were merged into `videoconvertscale` in this
> GStreamer version; `BR2_PACKAGE_GST1_PLUGINS_BASE_PLUGIN_VIDEOCONVERTSCALE=y`
> is already set and covers `waylandsink` caps negotiation. There is no separate
> `VIDEOCONVERT` symbol to enable.

---

## Architecture: the Wayland transport (Option A — pure C libwayland)

The QPA links `libwayland-client` directly and runs its own client, integrated
into Qt's event loop. It does **not** use `qt5wayland`'s client library at
runtime (that package is present only for the stock-path sanity check and header
availability). The transport is a **dedicated sibling class**,
`QEtnaviv2dWaylandTransport`, owned by the integration:

```
qpa/src/
  qetnaviv2dwaylandtransport.{h,cpp}   ← NEW — all libwayland client state
  qetnaviv2dintegration.{h,cpp}        ← owns the transport in wayland mode;
                                          skips evdev/libinput input setup
  qetnaviv2dbackingstore.{h,cpp}       ← flush() delegates to transport in
                                          wayland mode instead of pageFlip()
  qetnaviv2dscreen.{h,cpp}             ← KMS path untouched; isStub() routes away
  qetnaviv2dblitter.{h,cpp}            ← UNTOUCHED (D11)
```

`QEtnaviv2dWaylandTransport` owns:

- `wl_display`, `wl_registry`, `wl_compositor`
- `xdg_wm_base`, `xdg_surface`, `xdg_toplevel` (single fullscreen surface)
- `zwp_linux_dmabuf_v1`
- the **ARGB8888 / LINEAR BO ring** (3 deep) and its cached `wl_buffer`s
- `wl_surface.frame` callback state (throttling)
- `wl_buffer.release` back-pressure (`pickFree()` discipline)
- a `QSocketNotifier` on `wl_display_get_fd()` armed lazily on first commit
- (later, Milestone I) `wl_seat` + `wl_pointer` / `wl_touch` / `wl_keyboard`

### Zero-copy ARGB ring

The blitter's target BOs are dumb allocations exported as dmabuf via
`drmPrimeHandleToFD`. The transport allocates a **3-deep ring of exportable
ARGB8888/LINEAR BOs**, wraps each as a *persistent* `wl_buffer` (created once via
`zwp_linux_dmabuf_v1_create_params`, cached), and cycles them under
`wl_buffer.release`. The GC520 blitter renders straight into whichever ring BO
is currently free — **no per-frame ARGB memcpy**.

### Surface / role model

Single fullscreen `xdg_toplevel` (§Decision D7-role). Qt already composites all
child widgets, menus, popups, and dialogs into the top-level window's backing
store when there is no native window manager — one fullscreen surface renders the
entire Qt UI correctly. `xdg_popup` / subsurfaces are **not** implemented
(kiosk-appliance product; no product value). Every non-primary `QWindow` the QPA
is asked to create becomes a no-op / is composited by Qt into the primary surface.

### Event-loop integration

Single-threaded. A `QSocketNotifier` watches `wl_display_get_fd()`; on
activation the transport runs the safe read sequence
(`wl_display_prepare_read` → `wl_display_read_events` →
`wl_display_dispatch_pending`) to avoid the classic Wayland fd-starvation
deadlock. A flush hook on `QAbstractEventDispatcher::aboutToBlock` (and after
each `wl_surface_commit`) calls `wl_display_flush`. No dispatch thread — the
ring's `wl_buffer.release` handling stays on the main thread with the blitter and
Qt, so no locking (§Decision D7-loop).

The notifier is created **lazily on the first `commitFrame()` call** (after
`QApplication::exec()` enters the event loop) to avoid the Qt warning
`"QSocketNotifier: Can only be used with threads started with QThread"` and the
silent disabling that follows it.

### Frame pacing

Two guards, distinct jobs (§Decision D7-pace):

- **`wl_surface.frame` callback** governs *when* — vsync-aligned, coalescing
  redundant repaints. `BackingStore::flush()` in wayland mode marks the surface
  dirty; the actual `attach` + `commit` happens in the frame-callback handler.
  The first frame commits immediately to bootstrap the callback chain.
- **`wl_buffer.release`** governs *whether a buffer is free* — ring
  back-pressure. A Qt UI is bursty and mostly static; frame-callback throttling
  means at most one commit per vsync and none when nothing changed — correct for
  a low-power embedded kiosk.

### Input (wl_seat) — Milestone I, after A

Under Weston the QPA **must not** open `/dev/input/*` itself — Weston is the
input master (it owns libinput, incl. touchscreen calibration via `weston.ini`).
So in wayland mode `createInputHandlers()` is **skipped entirely**. Input instead
arrives over `wl_seat` → `wl_pointer` / `wl_touch` / `wl_keyboard`, which the
transport translates into `QWindowSystemInterface` events. **Implemented after
Milestone A** (§Decision D7-input).

---

## Protocol bindings

Generated at **build time** via `wayland-scanner`, XML from the sysroot
`wayland-protocols` datadir. Generated for the QPA build:

- `xdg-shell` — `xdg_wm_base` / `xdg_surface` / `xdg_toplevel`
- `linux-dmabuf-unstable-v1` — `zwp_linux_dmabuf_v1`

Core `wl_seat` / `wl_pointer` / `wl_touch` / `wl_keyboard` are in the core
`wayland.xml` provided by `libwayland-client` — no generation needed.

---

## Directory layout

```
wayland/
  plan.md                    ← this document
  Makefile                   ← builds all phase7 test harnesses
  tests/
    qt_ui_client/            ← minimal Qt app to exercise the QPA (milestone B)
    gst_dmabuf_video/        ← standalone C program: DRM dumb BO ring → appsrc → waylandsink (milestone A)
    gst_plugin_dmabuf_src/   ← GStreamer plugin (dmabufsrc): same BO logic as a proper GST element;
                                enables plain gst-launch-1.0 dmabufsrc … ! waylandsink (milestone A)
    verify_planes.sh         ← dri/0/state dual-promotion cross-check (milestone C)
  images/                    ← shared/symlinked NV12 .raw for the video clients
  build/                     ← build output (gitignored)
qpa/src/
  qetnaviv2dwaylandtransport.{h,cpp}   ← shipping transport (in place)
  (existing QPA files, minimally touched)
```

---

## Staged milestones

### Milestone B — single QPA Weston client (UI → hardware plane) ✓ DONE

**Gate B result (hardware-verified):**
- Qt UI visible fullscreen (1024×600), animated gradient, alpha-blend patches.
- Frames delivered via `linux-dmabuf-v1` (not SHM).
- `plane[38] AR24 zpos=1` — Qt UI on hardware overlay plane.
- `wl_buffer.release` cycling correctly through 3-slot ring.
- `QSocketNotifier` armed lazily; frame-callback chain pacing at vsync.

Notable bugs fixed during B:
- `QSocketNotifier` created before `QApplication::exec()` → silently disabled →
  `wl_buffer.release` events never dispatched → ring stalled, frame counter
  frozen. Fixed by lazy `armNotifier()` on first `commitFrame()`.

### Milestone C — two-plane arbitration (QPA UI + `test_weston` video) ✓ DONE

**Gate C result (hardware-verified, 220/220 frames PLANES_ONLY):**
```
plane[31] primary   NV12  zpos=0  (test_weston video)
plane[38] overlay   AR24  zpos=1  (qt_ui_client Qt UI)
```
- 100% `plane-only state composition` — no MIXED, no RENDERER_ONLY fallbacks.
- No shadow FB on primary (Weston does not render into the dumb BO when both
  client surfaces are on hardware planes).
- Correct zpos: video below, UI on top, ARGB blending visible.
- `--no-gradient` (semi-transparent Qt surface) confirmed: view-level alpha=1.0
  is what matters for plane eligibility, not pixel-level transparency.

Two Weston bugs found and fixed (now `0003-*.patch`):

1. **kiosk-shell background surface blocked PLANES_ONLY** — the solid-colour
   background view (no `wl_buffer`) triggered `force_renderer=true` in PLANES_ONLY
   mode where `renderer_ok=false`, immediately aborting the state proposal. Fix:
   skip no-buffer views with `continue`.

2. **Dual fullscreen clients: topmost forced to primary, second had no plane** —
   the `view_matches_entire_output && !scanout_has_view_assigned` guard refused
   overlay for any fullscreen view when primary was unassigned, causing the
   foreground view (Qt) to claim primary and leaving the background view (video)
   with no plane. Fix: count views with valid buffers; only apply the guard for a
   single client; with ≥2 clients let the topmost try overlay first.

### Milestone A — `gst_dmabuf_video` + `qt_ui_client` ✓ DONE

The stock `gst-launch-1.0 … ! waylandsink` path was investigated and **could
not be used directly** — three blockers were found and resolved:

1. **`waylandsink fullscreen=true` segfaults** — a NULL-deref in GStreamer
   1.22.9's xdg-output fullscreen path.  Workaround: omit `fullscreen=true`;
   kiosk-shell forces fullscreen anyway.

2. **`zwp_linux_dmabuf_v1` advertised zero formats** — Weston's
   `bind_linux_dmabuf` skipped format advertisement when
   `renderer->get_supported_formats == NULL` (pixman mode).  Fix: added
   `get_scanout_formats_hook` to `struct weston_compositor`; DRM backend sets
   it in `init_pixman()` to `get_scanout_formats_pixman_hook()` which returns
   the KMS plane format union.  `waylandsink` then sees
   `video/x-raw(memory:DMABuf), format={…NV12…}`.

3. **`rawvideoparse` produces system-memory NV12**, which `waylandsink` won't
   accept without `memory:DMABuf`.  No VPU GStreamer plugin is present on this
   target.  Resolution: wrote **`gst_dmabuf_video`** — a standalone C program
   (`wayland/tests/gst_dmabuf_video/gst_dmabuf_video.c`) that allocates a
   3-deep ring of DRM dumb BOs, reads .raw NV12 frames into them, exports each
   as a DMABuf fd, and pushes `memory:DMABuf` NV12 `GstBuffer`s via `appsrc`
   into a `waylandsink` pipeline.  A `GMainLoop` timer drives frame delivery
   at `--fps` rate; `gst_mini_object_weak_ref` clears the ring slot when
   GStreamer is done with the buffer.

4. **`drm_fb_get_from_dmabuf()` rejected `DRM_FORMAT_MOD_INVALID`** — Weston
   refused to import a DMABuf with no modifier because layout is then unknown.
   DRM dumb BOs are always linear; `gst_dmabuf_allocator` does not carry
   modifier metadata in GStreamer ≤ 1.22.  Fix: in `fb.c`, treat
   `DRM_FORMAT_MOD_INVALID` as `DRM_FORMAT_MOD_LINEAR` at import time.

These fixes are all in `0003-` (new commits in the weston git tree, to be
captured in `package/weston/`).

**Gate A result (hardware-verified):**
```
plane[31] primary   NV12  zpos=0  (gst_dmabuf_video)
plane[38] overlay   AR24  zpos=1  (qt_ui_client)
```
- 193/193 frames `plane-only state composition` — zero fallbacks.
- No shadow FB (XR24 absent from DRI state).
- Video animated (frame counter advancing, no ring stall).
- `verify_planes.sh`: 3/4 checks pass (the "≥2 active planes" check uses
  `FB_ID` grep which misses the new `fb=` format in debugfs — a script bug,
  not a hardware bug; both planes are active).

### Milestone I — input (wl_seat) — NEXT

Bind `wl_seat` in the transport; translate `wl_pointer` / `wl_touch` /
`wl_keyboard` → Qt events. Touchscreen: Weston owns libinput + calibration; the
client receives already-calibrated `wl_touch` coordinates. Validate: touch/mouse
in the Qt UI reaches widgets correctly.

**Gate I:** interactive Qt UI over video, touch/mouse working.

### Milestone IVI — ivi-shell (deferred, after I)

Replace kiosk-shell with ivi-shell for **deterministic, programmatic z-order
control** independent of client launch sequence. kiosk-shell is sufficient for
the current validated configuration (video first → primary, Qt second → overlay);
ivi-shell is required when layer assignment must be driven at runtime (e.g. swap
video/UI layers on a button press, or guarantee a fixed layout regardless of
startup order).

**What changes with ivi-shell:**

| Layer | Change | Owner |
|---|---|---|
| `weston.ini` | `shell=ivi-shell.so`; add `[ivi-shell]` section; configure `hmi-controller` surface-to-layer mapping | build system / config |
| `test_weston.c` / `waylandsink` wrapper | Bind `ivi_application` protocol; call `ivi_application_surface_create(iva, IVI_ID_VIDEO, wl_surface)` at startup | us (compositor test / gst wrapper) |
| QPA transport (`qetnaviv2dwaylandtransport.cpp`) | Bind `ivi_application`; call `ivi_application_surface_create(iva, IVI_ID_QT, wl_surface)` in `connect()` | us (QPA) |
| `hmi-controller.so` config | Map IVI surface IDs to IVI layers; set layer zpos in `weston.ini` or via `ivi_hmi_controller` protocol at runtime | us (config) or separate HMI process |

**Layer control responsibility:** the `hmi-controller` Weston plugin (loaded
in-process) owns `ivi_layout` — it calls `ivi_layout_layer_create()`,
`ivi_layout_surface_add_to_layer()`, `ivi_layout_layer_set_render_order()`. An
external HMI process can drive runtime layout changes via the `ivi_hmi_controller`
protocol. Without a controller, all surfaces share a default layer and z-order is
undefined.

The `0003-*.patch` plane-promotion fixes remain valid under ivi-shell — they are
in the DRM backend, below the shell layer.

---

## Decision log (Phase 7)

| ID | Decision | Rationale |
|----|----------|-----------|
| D7-topology | Staged: **(B)** single QPA client → **(C)** QPA + `test_weston` two-plane arbitration → **(A)** real `waylandsink`. ivi-shell deferred to after Milestone I. | The highest-risk unknown was the D11 QPA transport. Prove in isolation, then add arbitration with a known-good video partner, then swap in the real sink. ivi-shell deferred: kiosk-shell + `0003-*.patch` is sufficient for the validated configuration; ivi-shell only needed for runtime layer control. |
| D7-input | QPA binds `wl_seat` and translates Wayland input → Qt events; evdev/libinput path **disabled** in wayland mode. **Implemented after Milestone A.** | Under Weston the client must not open `/dev/input/*`. Sequenced after video to isolate transport risk first. |
| D7-buffer | **Zero-copy** ARGB8888/LINEAR BO ring (3 deep) exported as persistent `wl_buffer`s, cycled under `wl_buffer.release`. Blitter renders straight into the free ring BO. | etnaviv2d BOs are already exportable dmabuf; reuses D9b ring + `test_weston` transport; avoids a needless per-frame ARGB memcpy. |
| D7-role | Single fullscreen `xdg_toplevel`; Qt composites all child/popup content into it. No `xdg_popup`/subsurfaces. | Kiosk appliance. Qt already flattens child widgets into the top-level backing store with no WM. Multi-surface support = large lifecycle complexity, zero product value here. |
| D7-loop | Single-threaded `QSocketNotifier` on `wl_display_get_fd()`; notifier armed lazily on first `commitFrame()`; flush on `aboutToBlock`; `prepare_read`/`read_events`/`dispatch_pending` sequence. | Matches stock qtwayland; no threads means ring handling stays on the main thread — no locking. Lazy arming avoids the pre-exec-loop Qt notifier bug discovered at Milestone B. |
| D7-pace | `wl_surface.frame`-callback throttled commits + `wl_buffer.release` ring back-pressure. | Distinct jobs: frame callback = *when* (vsync, coalescing static UI); release = *whether a buffer is free*. Low-power behaviour for a bursty, mostly-static kiosk UI. |
| D7-zpos | MA35D1 planes are **format-equivalent** (RGB+YUV both). Target: **UI on higher-zpos plane (overlay) over video on lower-zpos (primary)**. Rely on Weston's natural stacking-to-plane mapping; pin only if wrong. | Format equivalence removes the plane-count-vs-format fight. Validated at Milestone C: 220/220 frames PLANES_ONLY, correct zpos. |
| D7-struct | Dedicated `QEtnaviv2dWaylandTransport` class owns all libwayland state; `BackingStore::flush()` delegates in wayland mode; KMS `Screen` path untouched; blitter/`EtnaBlittable` untouched. | Makes D11 literal — "adapted, not replaced; only transport switches". |
| D7-scanner | Generate protocol bindings at **build time** via `wayland-scanner`, XML from the sysroot `wayland-protocols` datadir. | Guarantees generated code matches the installed protocol version; keeps the QPA self-contained. |
| D7-weston-patches | Four patch areas in `package/weston/`. Fix 1 (0002): GBM+linux-dmabuf with pixman renderer. Fix 2 (0003): PLANES_ONLY no-buffer skip + dual-client overlay guard. Fix 3 (0003 continued): `get_scanout_formats_hook` in `weston_compositor` so pixman mode advertises KMS plane formats via `zwp_linux_dmabuf_v1`. Fix 4 (0003 continued): `DRM_FORMAT_MOD_INVALID` → `DRM_FORMAT_MOD_LINEAR` in `drm_fb_get_from_dmabuf()` for dumb BOs. | All fixes are in the DRM backend, below the shell layer — apply equally under kiosk-shell and ivi-shell. |
