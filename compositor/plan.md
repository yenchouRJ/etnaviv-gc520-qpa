# Plane-aware scanout for the etnaviv2d QPA — Implementation Plan

**Goal:** Let the Qt application target a *specific DRM plane* (primary,
overlay, or auto) via a QPA argument, so that a second producer (GStreamer /
FFmpeg) can drive video onto a different plane on the same CRTC without a
compositor. Design the plane-selection layer so a Wayland compositor
(Weston) can later take over plane assignment with **no change** to
`libetna2d` or the QPA blitter.

---

## Target hardware recap

The MA35D1 DRM driver (`drivers/gpu/drm/nuvoton`) exposes a single CRTC with
three hardware layers as DRM planes (`ma35_plane.c`):

| DRM plane type   | HW layer | Formats                         | Alpha / blend | Position |
|------------------|----------|---------------------------------|---------------|----------|
| `PRIMARY`        | Framebuffer | RGB + YUV (NV12, YUYV, P010…) | no (opaque, colorkey vs background) | fixed full-screen |
| `OVERLAY`        | Overlay  | RGB + YUV                       | yes — Porter-Duff `SRC_OVER`, global/coverage alpha, colorkey | positionable (`crtc_x/y/w/h`) |
| `CURSOR`         | Cursor   | XRGB8888, 32×32 (`MA35_CURSOR_SIZE`) | no        | positionable |

Key facts driving the design:

- **zpos is immutable** (`drm_plane_create_zpos_immutable_property`,
  ma35_plane.c:865): stacking order is fixed by the `layer-id` in the device
  tree — overlay/primary/cursor order cannot be reprogrammed at runtime.
- **Primary is not positionable** (`can_position = type != PRIMARY`,
  ma35_plane.c:284) and does **not** blend — it is the opaque background.
- **Overlay blends** with `SRC_OVER` and supports positioning + colorkey — it
  is the natural target for a translucent Qt UI floating over video.
- **The overlay plane can carry YUV**, so *either* split (UI-on-overlay /
  video-on-primary, **or** video-on-overlay / UI-on-primary) is electrically
  possible. We standardise on **UI → overlay, video → primary** because the
  overlay is the only plane that blends, and a UI needs per-pixel alpha over
  the video.

---

## Architecture

```
── No compositor (Phases 1–2) ──────────────────────────────────

Qt app (etnaviv2d QPA)            GStreamer / FFmpeg
   │  drmModeAtomicCommit            │  drmModeAtomicCommit
   │  (OVERLAY plane, ARGB, blend)   │  (PRIMARY plane, NV12/YUYV)
   └────────────────── /dev/dri/card0 ──────────────────────────
                        one DRM master (see lease note below)

── With Wayland compositor (Phase 3) ───────────────────────────

Qt app (wayland QPA)     GStreamer (waylandsink / dmabuf-import)
   │  wl_surface               │  wl_surface + linux-dmabuf-v1
   └───────────┐               └──────────────┐
               ▼                              ▼
         Weston compositor (DRM master)
            │  drmModeAtomicCommit
            │  (assigns planes from its pool)
            └──────── /dev/dri/card0
```

### The one-master problem

DRM allows exactly **one master** per `/dev/dri/card0`. Two independent
processes (Qt + GStreamer) cannot both do `drmModeAtomicCommit` on the same
card without coordination. Three ways to resolve this, in increasing
robustness:

1. **Single-owner (simplest, Phase 2 default).** One process is DRM master and
   commits *both* planes. The test program is that owner: it runs the Qt event
   loop and pulls decoded video frames from GStreamer via `appsink`
   (dmabuf-import), committing UI-on-overlay and video-on-primary in one
   atomic commit. GStreamer never touches KMS.

2. **DRM lease (Phase 2 stretch).** The Qt process is master, creates a
   `drmModeCreateLease` for the primary plane + its CRTC resources, and hands
   the lease fd to GStreamer (`kmssink`). Each side commits its own plane on
   its own fd. This is the honest multi-process model and the closest analogue
   to what a compositor does.

3. **Wayland compositor (Phase 3).** Weston is the sole master; Qt and
   GStreamer are Wayland clients. Plane assignment becomes Weston's job.

We implement (1) first because it needs no new IPC and exercises the full
plane path. The QPA plane-selection code is identical under all three — only
*who holds the master fd* changes.

### Wayland-forward invariants

To guarantee the QPA work survives the move to Weston, the plane-selection
logic must obey:

- **libetna2d and the blitter never learn about planes.** They only ever
  render into an `etna2d_surface`. Which plane that surface is scanned out on
  is decided entirely inside `QEtnaviv2dScreen`. Under Wayland the same
  surface becomes a `wl_buffer`; the blitter is unchanged.
- **Plane choice is resolved once at screen init**, from the QPA argument,
  and cached. No per-frame plane logic.
- **A "handoff" mode exists from day one**: when the QPA argument is `wayland`
  (or the env says a Wayland display is present), `QEtnaviv2dScreen` does *not*
  become DRM master and does *not* modeset — it is a no-op stub. This is the
  seam where the real Wayland QPA takes over later.

---

## QPA argument design

Qt platform plugins receive arguments via
`-platform etnaviv2d:key=value:key2=value2` (parsed in
`QEtnaviv2dIntegration`). Add one argument:

```
-platform etnaviv2d:plane=<mode>[,card=/dev/dri/cardN]
```

`plane` modes:

| Value       | Behaviour |
|-------------|-----------|
| *(absent)*  | **Default.** Auto: if `$WAYLAND_DISPLAY` is set → `wayland` handoff; else → `primary`. |
| `primary`   | Qt owns the primary plane, full-screen, opaque (current behaviour, legacy-compatible). |
| `overlay`   | Qt renders ARGB onto the overlay plane with `SRC_OVER`; leaves primary free for a video producer. |
| `auto`      | Same as absent but explicit. |
| `wayland`   | Handoff stub: screen does not become DRM master; reserved for the future Wayland QPA. |

Selection precedence: explicit QPA arg  >  `$QT_ETNAVIV2D_PLANE` env  >
`$WAYLAND_DISPLAY` presence  >  `primary` fallback.

---

## Implementation steps

### Step 0 — Preconditions (verify, no code)

- [ ] Confirm the driver advertises `DRM_CAP_ATOMIC` and the overlay plane is
      enumerated with `DRM_CLIENT_CAP_UNIVERSAL_PLANES` (already set at
      qetnaviv2dscreen.cpp:97). Dump with `modetest -M nuvoton -p`.
- [ ] Confirm overlay `possible_crtcs` includes our CRTC
      (`ma35_overlay_attach_crtc`, ma35_plane.c:891).
- [ ] Confirm the overlay accepts `DRM_FORMAT_ARGB8888` and the
      `porter-duff-blend-mode` / `source-alpha-mode` properties exist
      (ma35_plane.c:654–679). Note their property IDs for atomic commits.

### Step 1 — Convert the QPA scanout path from legacy to atomic KMS

The current screen uses `drmModeSetCrtc` + `drmModePageFlip`
(qetnaviv2dscreen.cpp:218,276), which implicitly binds the CRTC's *primary*
plane only. Plane targeting requires the atomic API.

- [ ] Add `drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)` next to the existing
      universal-planes cap.
- [ ] Enumerate planes with `drmModeGetPlaneResources` + `drmModeGetPlane`;
      classify each by its `type` property (PRIMARY/OVERLAY/CURSOR) and check
      `possible_crtcs`.
- [ ] Cache the object-property IDs needed for commits: on the plane
      (`FB_ID`, `CRTC_ID`, `SRC_{X,Y,W,H}`, `CRTC_{X,Y,W,H}`, `zpos`,
      `porter-duff-blend-mode`, `source-alpha-mode`) and on the CRTC
      (`ACTIVE`, `MODE_ID`) and connector (`CRTC_ID`).
- [ ] Replace `pageFlip()`'s `drmModePageFlip` with
      `drmModeAtomicCommit(..., DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, ...)`
      on the selected plane's `FB_ID`. Keep the existing vblank-event drain
      loop and the `m_flipMutex` / `m_shuttingDown` guards unchanged.
- [ ] Keep a legacy fallback path guarded by a build flag in case atomic is
      unavailable, but atomic is the primary path.

**Verify:** `plane=primary` on atomic reproduces the exact current behaviour
(all existing QPA tests still pass, no tearing, clean Ctrl-C).

### Step 2 — Plumb the `plane` argument through the plugin

- [ ] `QEtnaviv2dIntegration`: parse `plane=` and `card=` from the platform
      argument string; resolve the precedence chain (arg > env > wayland-probe
      > primary) into an enum `PlaneMode { Primary, Overlay, Wayland }`.
- [ ] Pass the resolved mode into `QEtnaviv2dScreen`'s constructor.
- [ ] `QEtnaviv2dScreen::initialize()`:
      - `Wayland` → return early as a no-op stub (log "handoff mode"); do not
        open card as master, do not modeset.
      - `Primary` → target the primary plane, full-screen, opaque (Step 1).
      - `Overlay` → target the overlay plane; set ARGB8888,
        `porter-duff-blend-mode = SRC_OVER`, `source-alpha-mode = None`
        (per-pixel alpha), position = full CRTC or a configurable rect.

**Verify:** `plane=overlay` shows the Qt UI blended over whatever the primary
plane holds (initially the background color); `plane=wayland` starts the app
without touching KMS.

### Step 3 — Overlay blend correctness

The overlay blends with the driver's `SRC_OVER` against the primary plane.
The QPA already produces `Format_ARGB32_Premultiplied` surfaces (matches the
GC520 PE). Confirm the *scanout* alpha survives to the overlay:

- [ ] Ensure the overlay framebuffer format is `DRM_FORMAT_ARGB8888`
      (not `XRGB8888`, which the current code hardcodes at
      qetnaviv2dscreen.cpp:183) so the alpha channel is honoured.
- [ ] Set `source-alpha-mode = None` so the driver uses the per-pixel alpha
      from the FB rather than the global color (ma35_plane.c:101–105,
      423–428).
- [ ] Decide whether the QPA surface must be *straight* or *premultiplied*
      alpha for the DE's overlay blend — the driver comment disables the
      pre-multiplied alpha mode (ma35_plane.c:104), so verify on hardware
      whether Qt's premultiplied output needs an un-premultiply pass or a
      `SRC` (not `SRC_OVER`) mode with the compositor doing coverage.

**Verify:** a semi-transparent Qt widget over a solid primary shows correct
edge blending, no dark/bright halo (the classic premultiply mismatch tell).

### Step 4 — Video producer + split-plane test program

Write `compositor/test_planes/` — a single-owner (model 1) program:

- [ ] Qt UI (etnaviv2d QPA, `plane=overlay`) draws an animated HUD with
      translucent panels.
- [ ] A GStreamer pipeline decodes video to NV12 dmabufs via `appsink`
      (`... ! v4l2/videoconvert ! video/x-raw,format=NV12 ! appsink`), and the
      program imports each dmabuf as a KMS FB (`drmModeAddFB2` with the dmabuf
      handle) and commits it to the **primary** plane.
- [ ] Both plane updates go in **one `drmModeAtomicCommit`** per frame, paced
      by a single page-flip event — this is why the single-owner model is the
      clean baseline: no cross-process flip contention.
- [ ] CLI flags: `--video=<file>`, `--ui-rect=WxH+X+Y`, `--fps`, and a
      `--separate-master` flag to exercise the DRM-lease path (Step 5).

**Pass criteria:**
- Video plays smoothly on the primary plane (YUV, no CPU copy).
- Qt HUD blends over it on the overlay with correct alpha.
- No tearing on either plane (single atomic commit → single vblank).
- Frame rate matches the video source; UI animation stays fluid.

### Step 5 — DRM lease path (multi-process, optional but Wayland-honest)

- [ ] Qt process (master) creates a lease for {primary plane, its FB props,
      CRTC subset} via `drmModeCreateLease` and passes the lease fd to a child
      GStreamer `kmssink` (`kmssink fd=<lease>` / `plane-id=<primary>`).
- [ ] Qt keeps and commits only the overlay; GStreamer commits only the
      primary on its leased fd.

**Verify:** identical visual result to Step 4, but now two processes each own
their plane — the true precursor to the Weston model, and proof that the QPA
plane isolation is real.

### Step 6 — Wayland-forward validation (design gate, no Weston yet)

- [ ] Confirm `plane=wayland` handoff stub is a clean no-op: the app runs, the
      QPA does not become DRM master, and `weston` could take the card.
- [ ] Document the mapping for the future Wayland QPA:
      `plane=overlay` (KMS) ⇄ `wl_surface` + `wl_subsurface` (Wayland);
      `plane=primary` video ⇄ `linux-dmabuf-v1` scanout candidate. Weston's
      DRM backend already promotes fullscreen dmabuf surfaces to hardware
      planes — so the same NV12 dmabuf that Step 4 puts on the primary can be
      handed to Weston and land on the same plane, unchanged.

---

## Deliverables

- `qpa/src/qetnaviv2dscreen.{h,cpp}` — atomic KMS, plane enumeration,
  plane-target selection, overlay blend config.
- `qpa/src/qetnaviv2dintegration.cpp` — `plane=`/`card=` argument parsing +
  precedence resolution.
- `compositor/test_planes/` — split-plane video+UI test (models 1 and 2).
- `compositor/plan.md` — this document.
- Update `qpa/qt-test.md` with a `test_planes` section (layout, plane map,
  run steps, pass criteria) mirroring the `test_kms_thread` entry.

## Risks / open questions

- **R1 — premultiplied vs straight alpha on the overlay.** Driver disables
  pre-multiplied alpha mode (ma35_plane.c:104). Resolve in Step 3; may force
  an un-premultiply blit or a mode change. **Highest-risk item.**
- **R2 — immutable zpos.** UI-on-overlay assumes overlay sits *above* primary
  in the DT layer order. Verify `layer-id` ordering in the device tree; if
  reversed, the split must be UI-on-primary / video-on-overlay, which breaks
  UI alpha (primary doesn't blend). Confirm before Step 2.
- **R3 — atomic vs legacy regression.** Existing tests assume legacy flip
  timing. Keep the `m_flipMutex`/`m_shuttingDown` invariants; re-run the full
  QPA suite after Step 1.
- **R4 — dmabuf import format.** Confirm the driver's `AddFB2` accepts the
  GStreamer decoder's NV12 stride/modifier layout without a copy.
