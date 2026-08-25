# Phase 7 — Two-client Wayland composition (Qt UI + video) under Weston

**Goal:** Run the etnaviv2d QPA as a **Weston Wayland client** that presents the
Qt UI to a hardware plane via `linux-dmabuf-v1`, alongside a **second client**
(GStreamer `waylandsink`) presenting NV12 video to the other plane — both
zero-copy, both hardware-promoted, Weston owning plane arbitration. This is
**D11** from the Phase 6 plan: the QPA is *adapted, not replaced* — only its
transport switches from direct KMS to Wayland; `libetna2d` / `EtnaBlittable` /
the GC520 blitter are untouched.

> **Status:** Planning complete. Phase 6 (Weston plane-promotion gate) is DONE
> and hardware-verified. All buildroot configs for Phase 7 are prepared.

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
                 │  to a hardware plane by zpos; drops its own shadow FB
                 │  when both surfaces fully cover the output
                 └──────── /dev/dri/card0
                                │
                    ┌───────────┴───────────┐
              plane (zpos hi): Qt UI    plane (zpos lo): video
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
| PRIMARY | 0 (bottom) | RGB + YUV | video (NV12) — *or* UI; format-equivalent |
| OVERLAY | 1 (middle) | RGB + YUV | Qt UI (ARGB) — *or* video; format-equivalent |
| CURSOR  | 2 (top)    | XRGB8888 32×32 | unused |

Because the two planes are format-equivalent, the **key validation point is
correct zpos-based plane allocation, not plane format capability**. The visual
target is **UI on the higher-zpos plane (overlay, blended) over video on the
lower-zpos plane (primary, opaque)** (§Decision D7-zpos).

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
- the **ARGB8888 / LINEAR BO ring** (2–3 deep) and its cached `wl_buffer`s
- `wl_surface.frame` callback state (throttling)
- `wl_buffer.release` back-pressure (`pickFree()` discipline from D9b / `test_weston`)
- a `QSocketNotifier` on `wl_display_get_fd()` for event-loop integration
- (later) `wl_seat` + `wl_pointer` / `wl_touch` / `wl_keyboard`

### Zero-copy ARGB ring

The blitter's target BOs are GBM/dumb allocations exported as dmabuf via
`drmPrimeHandleToFD`. The transport allocates a **2–3 deep ring of exportable
ARGB8888/LINEAR BOs**, wraps each as a *persistent* `wl_buffer` (created once via
`zwp_linux_dmabuf_v1_create_params`, cached), and cycles them under
`wl_buffer.release`. The GC520 blitter renders straight into whichever ring BO
is currently free — **no per-frame ARGB memcpy**. (Verify at impl time: the
blitter can target an arbitrary ring BO each frame; if the target is fixed at
surface creation, a blit-then-swap is needed — code detail, not a design change.)

### Surface / role model

Single fullscreen `xdg_toplevel` (§Decision D7-role). Qt already composites all
child widgets, menus, popups, and dialogs into the top-level window's backing
store when there is no native window manager — one fullscreen surface renders the
entire Qt UI correctly. `xdg_popup` / subsurfaces are **not** implemented
(kiosk-appliance product; no product value). Every non-primary `QWindow` the QPA
is asked to create becomes a no-op / is composited by Qt into the primary
surface.

### Event-loop integration

Single-threaded. A `QSocketNotifier` watches `wl_display_get_fd()`; on
activation the transport runs the safe read sequence
(`wl_display_prepare_read` → `wl_display_read_events` →
`wl_display_dispatch_pending`) to avoid the classic Wayland fd-starvation
deadlock. A flush hook on `QAbstractEventDispatcher::aboutToBlock` (and after
each `wl_surface_commit`) calls `wl_display_flush`. No dispatch thread — the
ring's `wl_buffer.release` handling stays on the main thread with the blitter and
Qt, so no locking (§Decision D7-loop).

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

### Input (wl_seat) — sequenced after video

Under Weston the QPA **must not** open `/dev/input/*` itself — Weston is the
input master (it owns libinput, incl. touchscreen calibration via `weston.ini`).
So in wayland mode `createInputHandlers()` (`qetnaviv2dintegration.cpp:256`) is
**skipped entirely** (no `QLibInputHandler` / evdev managers). Input instead
arrives over `wl_seat` → `wl_pointer` / `wl_touch` / `wl_keyboard`, which the
transport translates into `QWindowSystemInterface::handleMouseEvent` /
`handleTouchEvent` / `handleKeyEvent`. **This is implemented only after the video
and core Wayland-client bindings are proven** (§Decision D7-input, staging).

---

## Protocol bindings

Generated at **build time** via `wayland-scanner` (§Decision D7-scanner), XML
resolved from the sysroot `wayland-protocols` datadir
(`pkg-config --variable=pkgdatadir wayland-protocols`). Generated for the QPA
build:

- `xdg-shell` — `xdg_wm_base` / `xdg_surface` / `xdg_toplevel`
- `linux-dmabuf-unstable-v1` — `zwp_linux_dmabuf_v1`

Core `wl_seat` / `wl_pointer` / `wl_touch` / `wl_keyboard` are in the core
`wayland.xml` already provided by `libwayland-client` — no generation needed.
`wayland-scanner` is available as a host tool in the buildroot host sysroot
(since `wayland` is built). The QPA build (qmake `.pro` / Makefile) gets rules:
`wayland-scanner private-code $XML > foo-protocol.c` and
`wayland-scanner client-header $XML > foo-client-protocol.h`.

> Do **not** borrow `test_weston`'s already-generated files (bad layering, version
> drift). The QPA regenerates cleanly against its own sysroot.

---

## Directory layout

`wayland/` is a top-level sibling of `compositor/`, mirroring its structure. It
holds the **plan + disposable validation harnesses only**. The **shipping**
transport class lives in place under `qpa/src/`.

```
wayland/
  plan.md                    ← this document
  Makefile                   ← builds the phase7 test harnesses
  tests/
    qt_ui_client/            ← minimal Qt app to exercise the QPA (milestone B)
    verify_planes.sh         ← dri/0/state dual-promotion cross-check (milestone C)
  images/                    ← shared/symlinked NV12 .raw for the waylandsink client
  build/                     ← build output (gitignored)
qpa/src/
  qetnaviv2dwaylandtransport.{h,cpp}   ← shipping transport (in place)
  (existing QPA files, minimally touched)
```

---

## Staged milestones

The sequence is deliberately staged so each risk is isolated (§Decision D7-topology).

### Milestone B — single QPA Weston client (UI → hardware plane) — **FIRST**

The QPA currently has a `plane=wayland` stub — it connects to Weston but never
sends a frame. This milestone replaces the stub with a real Wayland client:
the QPA exports its ARGB render buffer as a dmabuf, submits it to Weston via
`linux-dmabuf-v1`, and drives the frame lifecycle (commit, release, next frame).
It is done alone first so that if anything in the Qt↔Weston handoff is wrong,
there is only one new moving part to debug.

Steps:

1. **B0 — scaffold.** `wayland/` directory, Makefile, tests dir already created.
   Add `wayland-scanner` rules to the QPA build for `xdg-shell` +
   `linux-dmabuf-v1`.
2. **B1 — transport class skeleton.** `QEtnaviv2dWaylandTransport`: connect
   display, bind registry globals (`wl_compositor`, `xdg_wm_base`,
   `zwp_linux_dmabuf_v1`), create fullscreen `xdg_toplevel`, wire the
   `QSocketNotifier` + `aboutToBlock` flush + safe read sequence.
3. **B2 — ARGB ring.** Allocate the 2–3 deep exportable ARGB8888/LINEAR BO ring;
   export each via `drmPrimeHandleToFD`; create persistent `wl_buffer`s via
   `zwp_linux_dmabuf_v1_create_params`; implement `pickFree()` +
   `wl_buffer.release`.
4. **B3 — flush integration.** In wayland mode, `BackingStore::flush()` blits into
   the current free ring BO (as today) and marks dirty; the frame-callback handler
   does `attach` + `damage_buffer` + `commit`; first frame bootstraps.
   `Screen`'s KMS path is not entered (`isStub()`).
5. **B4 — input disabled in wayland mode.** `createInputHandlers()` skipped when
   `PlaneMode::Wayland` (no evdev/libinput). App is navigable-blind for now —
   staging only.
6. **B5 — validate.** Minimal Qt app (`wayland/tests/qt_ui_client/`) with
   `QT_QPA_PLATFORM=etnaviv2d:plane=wayland` under a running Weston.

**Gate B:**
- **Hard:** Qt UI is visible on screen and frames are delivered to Weston via
  `linux-dmabuf-v1` (not SHM — SHM means the protocol path itself failed and the
  QPA fell back to shared memory instead of dmabuf export).
- **Observed, not a blocker:** whether Weston promotes the surface to a hardware
  plane or composites it in software. Weston's plane-assignment decision depends
  on surface coverage, modifier negotiation, and the xdg configure round-trip
  completing — these are properly verified in Milestone C, not here.

> Rationale: GL/software fallback at this stage is a Weston plane-assignment
> question, not a protocol question. The purpose of Milestone B is to confirm the
> Qt↔Weston handoff is correct. Plane promotion is gated at Milestone C, where
> both surfaces and the full scene are present for a meaningful check.

### Milestone C — two-plane arbitration (QPA UI + `test_weston` video)

Introduce two-plane arbitration using the **known-good** `test_weston` as the
video partner, so any arbitration failure is unambiguously Weston/kernel, not
`waylandsink`.

Steps:

1. **C0 — plane-availability observation.** Run `test_weston` (video) + the QPA
   UI client together. Observe in `/sys/kernel/debug/dri/0/state`: are **both**
   client surfaces on hardware planes, and does Weston **drop its shadow FB**
   when both fully cover the output?
2. **C1 — zpos verification.** Confirm the UI lands on the **higher-zpos** plane
   (overlay) and video on the **lower-zpos** plane (primary), by natural Weston
   stacking-to-plane mapping. If the observed order is wrong, add an explicit
   stacking/pin mechanism then (not before — §Decision D7-zpos).
3. **C2 — `verify_planes.sh`.** Script the `dri/0/state` dual-promotion +
   zpos cross-check.

**Gate C:** both surfaces hardware-promoted, correct zpos order, shadow FB
dropped. Any GL fallback = gate failure (software integration bug, not a hardware
limit — the planes are format-equivalent).

### Milestone A — real `waylandsink` video client

Swap the proven `test_weston` for the real GStreamer client:

```
gst-launch-1.0 filesrc location=earth_nv12_1024x600.raw \
  ! rawvideoparse format=nv12 width=1024 height=600 framerate=30/1 \
  ! waylandsink
```

Steps:

1. **A0 — waylandsink dmabuf probe.** Confirm `waylandsink` uses
   `zwp_linux_dmabuf_v1` (not SHM fallback) — check `dri/0/state` for the NV12
   plane. GStreamer ≥ 1.18 `waylandsink` supports dmabuf when the compositor
   advertises it (we do, post-Phase-6).
2. **A1 — two real processes.** Qt QPA client (UI) + `waylandsink` (video),
   both fullscreen, Weston arbitrating. Re-run `verify_planes.sh`.

**Gate A:** true product topology — two independent processes, both zero-copy
hardware-promoted, correct zpos. If `waylandsink` falls back to SHM, escalate
(may need `appsink` + manual dmabuf import, gated on VPU driver readiness — v2).

### Milestone I — input (wl_seat) — after A (or after B if prioritized)

Bind `wl_seat` in the transport; translate `wl_pointer` / `wl_touch` /
`wl_keyboard` → Qt events. Touchscreen: Weston owns libinput + calibration; the
client receives already-calibrated `wl_touch` coordinates. Validate: touch/mouse
in the Qt UI reaches widgets correctly.

**Gate I:** interactive Qt UI over video, touch/mouse working.

---

## Decision log (Phase 7)

| ID | Decision | Rationale |
|----|----------|-----------|
| D7-topology | Staged: **(B)** single QPA client → **(C)** QPA + `test_weston` two-plane arbitration → **(A)** real `waylandsink`. First milestone is a single QPA Weston client. | The highest-risk unknown is the D11 QPA transport (`isStub()` → `linux-dmabuf-v1`). Prove it in isolation; then add arbitration with a known-good video partner; then swap in the real sink. Any failure is attributable to one layer at a time. |
| D7-input | QPA binds `wl_seat` and translates Wayland input → Qt events; evdev/libinput path **disabled** in wayland mode. **Implemented after** video + core bindings are proven. | Under Weston the client must not open `/dev/input/*` (Weston is input master, owns touchscreen calibration). Binding `wl_seat` in the Option-A client loop is the self-contained continuation, mirroring stock qtwayland. Sequenced after video to isolate transport risk first. |
| D7-buffer | **Zero-copy** ARGB8888/LINEAR BO ring (2–3 deep) exported as persistent `wl_buffer`s, cycled under `wl_buffer.release`. Blitter renders straight into the free ring BO. | etnaviv2d BOs are already exportable dmabuf; reuses D9b ring + `test_weston` transport verbatim; avoids a needless per-frame ARGB memcpy for a UI surface. |
| D7-role | Single fullscreen `xdg_toplevel`; Qt composites all child/popup content into it. No `xdg_popup`/subsurfaces. | Kiosk appliance (kiosk-shell, D20). Qt already flattens child widgets/menus/dialogs into the top-level backing store with no window manager. Multi-surface support = large lifecycle complexity, zero product value here. |
| D7-loop | Single-threaded `QSocketNotifier` on `wl_display_get_fd()`; flush on `aboutToBlock`; `prepare_read`/`read_events`/`dispatch_pending` sequence. | Matches stock qtwayland; no threads means `wl_buffer.release`/ring handling stays on the main thread with the blitter — no locking. Avoids the Wayland fd-starvation deadlock. |
| D7-pace | `wl_surface.frame`-callback throttled commits (vsync-aligned, coalesced) + `wl_buffer.release` ring back-pressure. | Distinct jobs: frame callback = *when* (vsync, coalescing static UI); release = *whether a buffer is free*. Correct, low-power behavior for a bursty, mostly-static kiosk UI; matches `QWindow::requestUpdate`. |
| D7-zpos | MA35D1 planes are **format-equivalent** (RGB+YUV both). Only immutable constraint is zpos. Target: **UI on higher-zpos plane (overlay) over video on lower-zpos (primary)**. Rely on Weston's natural stacking-to-plane mapping; verify in (C); add explicit pin only if observed order is wrong. GL fallback = gate failure (software issue, not hardware). Weston shadow FB drops when both surfaces cover the output. | Format equivalence removes the plane-count-vs-format fight. The real validation is correct zpos allocation, verified via `/sys/kernel/debug/dri/0/state`. No pinning mechanism pre-built for a mapping that may already be correct. |
| D7-struct | Dedicated `QEtnaviv2dWaylandTransport` class owns all libwayland state; `BackingStore::flush()` delegates in wayland mode; KMS `Screen` path untouched; blitter/`EtnaBlittable` untouched. | Makes the D11 principle literal — "adapted, not replaced; only transport switches". Sibling transports never interleave; the two paths stay clean. Natural home for the later `wl_seat` input code. |
| D7-scanner | Generate protocol bindings at **build time** via `wayland-scanner`, XML from the sysroot `wayland-protocols` datadir. | Guarantees generated code matches the installed protocol version (avoids `DRM_FORMAT_MOD_INVALID`-class surprises); keeps the QPA self-contained; canonical approach. Not borrowed from `test_weston` (bad layering / drift). |
| D7-layout | Top-level `wayland/` sibling of `compositor/`, mirroring its structure — plan + disposable validation harnesses only. Shipping transport class in place under `qpa/src/`. | The transport is integral shipping QPA code, not a throwaway test. `wayland/` holds only Phase 7 planning + validation scaffolding, same split as Phase 6's `compositor/` + `test_weston`. |

---

## Open items to resolve at implementation time (not blocking)

- **Blitter target flexibility** — can the GC520 blitter render into an arbitrary
  ring BO each frame, or is its target fixed at surface creation? If fixed, a
  blit-then-swap wrapper is needed (code detail; D7-buffer still holds).
- **Weston shadow-FB drop** — confirm Weston's DRM backend drops its compositor
  shadow FB when both client surfaces fully cover the output (observed for a
  single client in Phase 6; verify for two in milestone C0).
- **Natural zpos mapping** — whether Weston stacks the UI above the video by
  default under kiosk-shell (creation/focus order). Observed in C1; pin only if
  wrong.
- **`waylandsink` dmabuf vs SHM** — confirm dmabuf path in A0; SHM fallback
  escalates to `appsink` + manual import (v2, VPU-gated).
```