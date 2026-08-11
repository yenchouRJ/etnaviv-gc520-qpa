# Plane-aware scanout for the etnaviv2d QPA — Implementation Plan

**Goal:** Let a single Qt process drive both its **GUI** (on the overlay plane,
blended) and its **video stream** (on the primary plane) through the etnaviv2d
QPA, using one atomic KMS commit. Design the plane layer so a Wayland
compositor (Weston) can later own multi-process plane assignment with **no
change** to `libetna2d` or the QPA blitter.

> **Status:** Steps 0–3 complete and hardware-verified. Step 4 is next.

---

## The two-state model (this is the whole design)

```
── One process (Phases 1–2, this plan) ─────────────────────────

  Qt app (etnaviv2d QPA) — owns GUI *and* video
     │  GStreamer appsink → memcpy NV12 → primary BO ring
     │  Qt render        → ARGB overlay BO
     │
     └── ONE drmModeAtomicCommit (overlay FB + primary FB) ──────
                         │
                    /dev/dri/card0   (Qt is sole DRM master)

── Multiple processes (Phase 3, future) ────────────────────────

  Qt app (wayland QPA)     GStreamer (waylandsink / dmabuf-import)
     │  wl_surface               │  wl_surface + linux-dmabuf-v1
     └───────────┐               └──────────────┐
                 ▼                              ▼
           Weston compositor (sole DRM master)
              │  drmModeAtomicCommit (assigns planes from its pool)
              └──────── /dev/dri/card0
```

**There is deliberately no in-between.** Two raw-KMS processes cannot both hold
DRM master on `card0` — the kernel grants master to the first opener; a second
`open()` shares the master object but has `is_master=0` and its commits are
rejected with `EACCES`.  The correct path for multi-process plane arbitration is
**DRM leasing** (kernel 6.6 supports `DRM_IOCTL_MODE_CREATE_LEASE`): the master
carves out a set of objects (plane + CRTC) and hands a leased fd to the client,
which then has exclusive rights.  That is what a Wayland compositor does.  For a
single-process program, sharing the one master fd directly is both simpler and
correct — no leasing needed.  The DRM-lease multi-process path is therefore a
**non-goal** (D4): either one process owns everything, or a Wayland compositor
owns arbitration.

**The GPU-op layer is invariant across all phases.** `libetna2d` and
`EtnaBlittable` only ever render into an `etna2d_surface` — they never learn
about planes, KMS, or Wayland. Only the *transport* layer of
`QEtnaviv2dScreen` changes:

| Phase | Render (invariant) | Transport / scanout |
|-------|--------------------|---------------------|
| No compositor | `EtnaBlittable` → `etna2d_surface` | dmabuf → `drmModeAtomicCommit` (direct KMS) |
| Weston | `EtnaBlittable` → `etna2d_surface` | dmabuf → `wl_surface` + `linux-dmabuf-v1` → `wl_surface_commit`; Weston picks the plane |

The etnaviv2d QPA remains the **sole holder of GC520 DE knowledge** in both
worlds. Weston owns *arbitration* (which plane, multi-client), not *rendering*.

---

## Target hardware (confirmed against sources)

MA35D1 DRM driver (`drivers/gpu/drm/nuvoton`), single CRTC, three planes:

| Plane   | zpos | layer-id (DT) | Formats                    | Blend | Position |
|---------|------|---------------|----------------------------|-------|----------|
| PRIMARY | 0 (bottom) | 0 | RGB + YUV (NV12, YUYV, P010…) | no — opaque | fixed full-screen |
| OVERLAY | 1 (middle) | 1 | RGB + YUV                  | **yes** — Porter-Duff `SRC_OVER` | positionable |
| CURSOR  | 2 (top)    | 2 | XRGB8888, 32×32            | no    | positionable |

Confirmed facts driving the design:

- **zpos order (R2 — closed).** DT `ma35d1.dtsi:1774–1789` sets
  `layer-id`: primary=0, overlay=1, cursor=2. `ma35_layer_create` maps
  `zpos = index` (ma35_plane.c:865), immutable. **Overlay (zpos 1) is above
  primary (zpos 0)** → UI-on-overlay / video-on-primary is correct. Not a
  risk.
- **Overlay blends premultiplied `SRC_OVER` (R1 — closed by spec).** The
  MA35D1 display spec documents `SRC_OVER` on **premultiplied** alpha with the
  standard formula `out = src + dst·(1−src.a)`. This matches Qt's
  `Format_ARGB32_Premultiplied` output exactly — **zero conversion** in the
  overlay path. `source-alpha-mode = None` makes the driver honour the FB's
  per-pixel alpha (ma35_plane.c:101–105, 423–428).
- **Primary is opaque and not positionable** (`can_position = type != PRIMARY`,
  ma35_plane.c:284) — the correct video background.
- **NV12 on planes (R4 — sufficient for v1).** `MA35_FORMAT_NV12` is in the
  shared plane format list (ma35_plane.c:43, validated :164) for both primary
  and overlay. The plane *accepts* NV12; zero-copy dmabuf import is a **v2**
  concern gated on the VPU/KMS driver.

### Split-plane assignment (locked)

**UI → overlay (blends), video → primary (opaque NV12).** The overlay is the
only plane that blends, and a UI needs per-pixel alpha over the video. The DT
zpos order supports this natively.

---

## Wayland-forward invariants (must hold through all phases)

- **libetna2d and the blitter never learn about planes.** They only render
  into an `etna2d_surface`. Which plane it scans out on is decided entirely in
  `QEtnaviv2dScreen`. Under Wayland the same surface becomes a `wl_buffer`;
  the blitter is unchanged.
- **Plane choice is resolved once at screen init** from the QPA argument, then
  cached. No per-frame plane logic.
- **A `wayland` handoff stub exists from day one:** when the QPA argument is
  `wayland` (or `$WAYLAND_DISPLAY` is set), `QEtnaviv2dScreen`'s **direct-KMS**
  transport is disabled — it does not become DRM master and does not modeset.
  This is the seam where the QPA's transport layer is later switched to the
  Wayland client path (D11); the blitter/`libetna2d` side is untouched.
- **The `attachPrimaryFb` hook is the ancestor of a Wayland scanout
  candidate.** In single-owner mode it stages an NV12 FB into the QPA's
  commit; under Weston the same NV12 buffer becomes a `linux-dmabuf-v1`
  surface that Weston promotes to a hardware plane. Same buffer, different
  arbiter.

---

## QPA argument

```
-platform etnaviv2d:plane=<mode>[,card=/dev/dri/cardN]
```

| Value      | Behaviour |
|------------|-----------|
| *(absent)* | **Default.** Auto: `$WAYLAND_DISPLAY` set → `wayland` handoff; else `primary`. |
| `primary`  | Qt owns primary, full-screen, opaque. Legacy-compatible (also the atomic A/B oracle). |
| `overlay`  | Qt renders full-screen ARGB8888 on the overlay with premultiplied `SRC_OVER`; leaves primary free for the in-process video path. Runnable standalone (see D6). |
| `auto`     | Explicit form of *(absent)*. |
| `wayland`  | Handoff stub: no DRM master, no modeset. Reserved for the future Wayland QPA. |

Precedence: explicit QPA arg > `$QT_ETNAVIV2D_PLANE` env > `$WAYLAND_DISPLAY`
presence > `primary` fallback.

---

## Overlay configuration (fully pinned)

- FB format: `DRM_FORMAT_ARGB8888` (**not** the current hardcoded `XRGB8888`).
- `source-alpha-mode = None` — per-pixel alpha from the FB.
- `porter-duff-blend-mode = SRC_OVER`.
- Source pixels: **premultiplied** (Qt native, no conversion — R1 spec-confirmed).
- Geometry: **full-screen** overlay; transparent where no widget draws, video
  shows through. `geometry()` stays full-screen (Qt sees a normal full screen).

---

## The single-commit / attach-hook contract

`QEtnaviv2dScreen` is the **sole KMS committer**. Both planes land in one
atomic commit → one page-flip event → tear-free plane sync and the existing
`m_flipMutex` invariant is preserved.

### `attachPrimaryFb()` — latched staging + three-buffer discipline

```
3 primary NV12 buffers, each with one bit: ON_SCREEN (else free).

video thread (GStreamer appsink cb):
    pick a buffer that is NOT on_screen AND NOT the current staged one
    memcpy NV12 → that buffer                 ← DC is never reading it
    attachPrimaryFb({fbId, ...})              ← flip lock: set staged = buffer,
                                                 request wake-up (D12), return.
                                                 Never blocks on vblank.

QPA flip thread (pageFlip(), under m_flipMutex):
    read staged buffer (last-write-wins)
    drmModeAtomicCommit(overlay FB + staged primary FB, PAGE_FLIP_EVENT | NONBLOCK)
    mark that primary buffer ON_SCREEN

flip-event drain (existing handler):
    clear ON_SCREEN on the buffer whose commit just completed → it becomes free
```

- **Decoupled:** the GStreamer thread never waits on display timing.
- **Correct by construction (D9b, revised):** the producer picks *only* a
  buffer that is neither `on_screen` (the DC is scanning it) nor the currently
  `staged` one. It therefore can **never overwrite the buffer the display
  controller is reading** — the failure the earlier "3-deep ring" and the naïve
  "2-BO swap" both had (a fast producer running >1 frame ahead within one
  vblank wraps onto the on-screen buffer). Three buffers with a single
  `on_screen` bit (cleared on the flip event) is the minimal race-free design —
  one bool per buffer, no state machine.
- **Implicit drop / repeat:** producer too fast → it overwrites a free buffer,
  newest-wins (old frame dropped). Consumer too fast → `staged` unchanged, the
  same frame is re-committed (repeat). Both degenerate cases are automatically
  correct.
- **Overlay unchanged:** overlay keeps its existing **2-deep** double-buffer
  (Qt renders it inline before the flip, so no extra slack needed).
- **Shared release:** one atomic commit → one `DRM_MODE_PAGE_FLIP_EVENT`. When
  it drains, the overlay back-buffer flips *and* the just-committed primary
  buffer's `on_screen` bit is cleared — a single completion advances both
  sides.

### Video-driven present (the second commit trigger)

Because `QEtnaviv2dScreen` is the **sole committer**, the primary (video) plane
reaches the display only through the atomic commit that `pageFlip()` issues.
There are two independent producers that must be able to cause a commit:

- **The UI (overlay)** — Qt drives a commit whenever a window repaints
  (`flush()` → `pageFlip()`), i.e. on animation, input, or damage.
- **The video (primary)** — a new decoded frame must also be able to cause a
  commit, *independently of UI activity*.

If only the UI could trigger commits, a **static UI** (no animation, no input,
no dirty region) would produce no repaints → no `flush()` → no `pageFlip()` →
no commit, and the video would sit on its last committed frame while the
GStreamer thread kept staging new ones. So by design the video producer is a
**first-class present trigger**, symmetric with the UI.

**Mechanism (D12).** `attachPrimaryFb()`, after staging its buffer, requests a
present on the GUI side:

- Post a **coalesced** update to the platform window
  (`QWindow::requestUpdate()` / an update event on the QPA window),
  marshalled to the **GUI thread** — the GStreamer thread must never call
  `QWindow` APIs directly.
- Coalesce to **≤ one request per vblank interval**, not one per decoded
  frame: a fast producer must not flood the event loop or drive commits faster
  than the display refresh. Last-write-wins on the staged buffer means the next
  commit always carries the newest frame regardless of how many were dropped.
- If a flip is already in flight, do nothing — the pending commit will pick up
  the latest staged buffer when it runs.

This preserves the "producer never blocks on vblank" property (D9) while making
the video advance on its own cadence. It is also **Wayland-forward**: under
Weston the same request becomes a present driven by a `wl_surface` frame
callback, so "producer requests a present" maps directly across phases.

---

## Implementation steps

### Step 0 — Preconditions (verify, no code) — **DONE**

Verified against `compositor/build/modetest.log` (`modetest -M ma35-drm -p`):

- [x] Planes enumerate under `DRM_CLIENT_CAP_UNIVERSAL_PLANES`: **3 planes** —
      **31 Primary** (type 1, zpos 0), **38 Overlay** (type 0, zpos 1),
      **49 Cursor** (type 2, zpos 2). All zpos `immutable` → **R2 confirmed:
      overlay (1) above primary (0)**. Single CRTC **52**.
- [x] `possible_crtcs = 0x00000001` on all three planes → all attach CRTC 52.
- [x] Overlay (38) exposes `porter-duff-blend-mode` (enum, **SRC_OVER=3**,
      default 3) and `source-alpha-mode` (enum, **None=0**, default None) →
      **R1 wiring confirmed**.
- [x] **NV12** listed in the format set of **both** primary and overlay →
      **R4 confirmed**. Overlay also has `swizzle`, `colorkey-*`,
      `destination-alpha-mode`, `source/destination-global-color`.
- [x] Cursor (49) is **XR24 only**, 32×32 (unused by this plan).

**Contract carried into Step 1 (from the dump):**
- The blend props (`porter-duff-blend-mode`=40, `source-alpha-mode`=41,
  `destination-alpha-mode`=42) exist **only on the overlay plane**; the primary
  has none. Cache blend props on the **overlay object only**; the primary path
  must never touch them.
- Property IDs are **per-object and hardware-assigned** (e.g. `swizzle` is 33 on
  primary, 45 on overlay; `zpos` is 37/48/51). **Always resolve property IDs by
  name at runtime per object** — never hardcode an ID.
- The QPA opens the card **by path** (`m_cardPath`, qetnaviv2dscreen.cpp:92), so
  there is no `-M ma35-drm` driver-name coupling; `card=` just needs to resolve
  to the ma35-drm node (single GPU node).

### Step 1 — Legacy → atomic KMS (build-time oracle kept) — **DONE**

All 7 QPA tests (`test_blitter/screen/integration/benchmark/slideshow/glyph/kms_thread`)
passed on hardware in both atomic (default) and legacy (`LEGACY_KMS=1`) builds.
No regressions. R3 closed.

- [x] Add `drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)`.
- [x] Enumerate planes (`drmModeGetPlaneResources` + `drmModeGetPlane`),
      classify by `type`, check `possible_crtcs`.
- [x] Cache object-property IDs **by name, per object** (IDs are
      hardware-assigned and differ per plane — see Step 0 contract): plane
      (`FB_ID`, `CRTC_ID`, `SRC_*`, `CRTC_*`, `zpos`), CRTC (`ACTIVE`,
      `MODE_ID`), connector (`CRTC_ID`). Cache the blend props
      (`porter-duff-blend-mode`, `source-alpha-mode`) on the **overlay object
      only**; assert they are absent on the primary.
- [x] Replace `pageFlip()`'s flip with `drmModeAtomicCommit(...,
      PAGE_FLIP_EVENT | ATOMIC_NONBLOCK, ...)`. Keep the vblank-event drain
      loop and the `m_flipMutex` / `m_shuttingDown` guards **unchanged**.
- [x] Replace `cleanup()`'s `drmModeSetCrtc` restore (qetnaviv2dscreen.cpp:334)
      with a `drmModeAtomicCommit(ALLOW_MODESET)` that sets `ACTIVE=0` (or
      restores the saved mode blob). **Required:** once `DRM_CLIENT_CAP_ATOMIC`
      is set, mixing legacy `drmModeSetCrtc` calls on the same CRTC returns
      `EINVAL`.
- [x] Keep legacy code behind a **build-time `#ifdef`** as a frozen A/B
      regression oracle (`plane=primary` only). Atomic is the default and the
      only plane-capable path. (Display driver is owned in-house; atomic is
      always available — no runtime fallback, D7.)

### Step 2 — Plumb the `plane` argument — **DONE**

- [x] `QEtnaviv2dIntegration`: parse `plane=` / `card=`; resolve precedence
      (arg > `QT_ETNAVIV2D_PLANE` env > `$WAYLAND_DISPLAY` probe > primary)
      into `PlaneMode { Primary, Overlay, Wayland }`.
- [x] Pass the mode into `QEtnaviv2dScreen` (new constructor argument).
- [x] `initialize()`:
      - `Wayland` → return early, no-op stub (log "handoff mode"); no master,
        no modeset. Nominal geometry set so Qt has a valid screen.
      - `Primary` → primary plane, full-screen, opaque (Step 1 path).
      - `Overlay` → overlay plane selected by `DRM_PLANE_TYPE_OVERLAY`
        (blend props wired in Step 3).

**Bugs found and fixed during testing:**
- (B) `plane=wayland`: backing store spammed "no scanout back surface" warnings.
  Fixed: `isStub()` accessor on screen; flush silently skips when stub.
- (A) `plane=primary` after `plane=overlay`: tearing / lost vsync.
  Root cause: **driver bug** in `ma35_plane_atomic_disable` — the shared
  callback only cleared `MA35_PRIMARY_ENABLE`, never `MA35_OVERLAY_ENABLE`,
  so the overlay hardware layer stayed enabled with a stale DMA address after
  the overlay session exited. Fixed in
  `drivers/gpu/drm/nuvoton/ma35_plane.c`: branch on `drm_plane->type` to
  clear `MA35_OVERLAY_CONFIG / MA35_OVERLAY_ENABLE` for the overlay case,
  matching the `atomic_update` pattern.

### Step 3 — Overlay blend correctness — **DONE**

**What changed from the original goal:** Step 3 was originally scoped as a
standalone `plane=overlay` test over the driver background colour (D6).  During
implementation it became a **single-process, two-plane test** — one
`QApplication` driving both the primary background (Qt widget, ~60 fps) and the
overlay (direct `QEtnaviv2dScreen` instance, ~60 fps) — and surfaced a
fundamental DRM ownership constraint that reshaped how multi-plane use works in
this codebase.

**Why two processes don't work (and why we didn't use DRM leasing):**

The Linux kernel's DRM object model (`drm_mode_object_find`, 6.6) requires that
every atomic commit referencing a CRTC, connector, or plane be made from a fd
that is either the **DRM master** or holds a **DRM lease** for those objects.
A second `open()` of `card0` — even from the same process — gets
`file_priv->is_master = 0` (it shares the master object but is not master
itself).  Any commit from that fd that references a CRTC or plane returns
`EACCES`.

DRM leasing (`DRM_IOCTL_MODE_CREATE_LEASE`, fully supported in 6.6) is the
correct multi-process answer: the master carves out a subset of objects and
passes a leased fd to the client process, which then has exclusive rights over
those objects.  That is what a Wayland compositor does (Phase 3, D4).  For a
single-process program it is unnecessary overhead — sharing the one master fd
directly is both simpler and correct.

**The fix — `sharedKfd` constructor parameter:**

`QEtnaviv2dScreen` now accepts an optional `sharedKfd` fd.  When provided it
skips `open()` and does not close the fd on destruction (caller owns lifetime).
Both screen instances commit through the same `struct drm_file` in the kernel —
the one that has `is_master = 1` — so both are permitted without leasing.

**Plane updates and vsync:** each `pageFlip()` issues its own
`drmModeAtomicCommit(..., NONBLOCK | PAGE_FLIP_EVENT)`.  Since both planes share
CRTC 52 they share the same vblank interrupt; if both commits land before the
deadline they apply atomically in the same vblank.  The test drives them with
independent timers (not coordinated) — Step 4 replaces this with a **single
commit containing both plane FB_IDs** so they are always vblank-locked.

**QPA changes (all in `qetnaviv2dscreen.{h,cpp}`):**

- `sharedKfd = -1` constructor param + `m_ownsKfd` flag: shared fd is not
  opened or closed by the borrowing instance.
- `etnaCtx()` / `etnadev()` read accessors: needed to pass the GPU context to
  the sibling overlay screen.
- FB format: `DRM_FORMAT_ARGB8888` for overlay FBs (was hardcoded `XRGB8888` —
  without this the alpha byte is ignored by the hardware).
- Overlay modeset skips CRTC/connector props and uses `flags=0` (plane-only
  commit); the CRTC is already active from the primary commit and only a
  topology-changing commit requires master privilege.
- `porter-duff-blend-mode = 3` (SRC_OVER) and `source-alpha-mode = 0`
  (per-pixel) added to the overlay modeset req (these were resolved but never
  set — the plane was compositing as opaque).

**Test: `compositor/tests/test_alpha_blend/`**

Single binary, single process.  QPA (`plane=primary`) drives a background image
widget at ~60 fps.  `OverlayDriver` directly instantiates a second
`QEtnaviv2dScreen` (overlay, shared fd), paints into the BO via `QPainter` +
`memcpy`, and calls `pageFlip()` at ~60 fps.

| Patch | Content | Pass criterion |
|-------|---------|----------------|
| T1 | α=0 transparent | Primary image shows through; no overlay tint |
| T2 | α=128 premultiplied red | 50% red blended over primary |
| T3 | α=255 opaque white | Primary completely blocked |
| HUD | α=160 animated cyan box, live fps counter | Smooth animation; correct semi-transparent blend |

All criteria passed on hardware at 59–60 fps.

### Step 4 — In-process video path + split-plane test program

`compositor/test_planes/` — single-owner, Qt is the only DRM master.

- [ ] Qt UI (etnaviv2d QPA, `plane=overlay`) draws an animated translucent HUD,
      full-screen.
- [ ] GStreamer decodes to NV12 via `appsink` **into CPU memory**; the program
      `memcpy`s each frame into a **free** primary buffer (one that is neither
      `on_screen` nor `staged` — **3 buffers + `on_screen` bit**, D9b — no naïve
      ring/swap), `drmModeAddFB2`s it, and calls `attachPrimaryFb()`.
- [ ] The QPA folds the staged primary FB into its **single** atomic commit
      (D3/D9). No separate committer.
- [ ] `attachPrimaryFb()` drives a **coalesced GUI-thread repaint** so the
      video advances even when the UI is static (D12) — verify with a static
      HUD: the video must keep playing.
- [ ] CLI: `--video=<file>`, `--fps`. (`--ui-rect` sub-rectangle is **v2**.)

**Pass criteria:**
- Video plays on the primary (NV12), Qt HUD blends over it on the overlay.
- No tearing on either plane (single commit → single vblank).
- **Video keeps playing when the UI is fully static** (no animation/input) —
  the D12 wake-up path works.
- **Fast decode with a static UI shows no tearing on the primary** — the D9b
  `on_screen`-bit discipline prevents the producer from overwriting the
  scanned-out buffer.
- Frame rate tracks source; UI animation fluid.
- If live decode + memcpy perf is unacceptable, fall back to **pre-decoded raw
  NV12 frames** (like `test_kms_thread`'s frame cache) — accepted in D2.

### Step 5 — Wayland-forward validation (design gate, no Weston yet)

- [ ] Confirm `plane=wayland` handoff stub is a clean no-op: app runs, QPA's
      direct-KMS transport is disabled, `weston` could take the card.
- [ ] Document the future transport mapping (render layer unchanged):
      `plane=overlay` (KMS) ⇄ `wl_surface`/`wl_subsurface`;
      `plane=primary` video (attachPrimaryFb NV12) ⇄ `linux-dmabuf-v1` scanout
      candidate. Weston's DRM backend already promotes fullscreen dmabuf
      surfaces to hardware planes — the same NV12 buffer lands on the same
      plane, unchanged. The etnaviv2d QPA is **adapted** to emit `wl_surface`
      commits; `EtnaBlittable`/`libetna2d` remain the GPU-op layer (D11).

### Step 6 — *(Optional)* SW-render mode for GPU vs CPU benchmark

**Purpose:** isolate the GC520 DE contribution by holding the display path
constant.  The comparison `our SW mode` vs `our GPU mode` answers the question
*"is the GC520 DE faster than the CPU for 2D ops?"* cleanly.  Comparing against
linuxfb conflates two variables (rendering engine **and** display path); this
step removes the display-path variable entirely.

**What changes (small, self-contained):**

| Component | Change |
|-----------|--------|
| `QEtnaviv2dScreen` | `mmap` the dumb BOs at allocation (`DRM_IOCTL_MODE_MAP_DUMB`); expose the CPU pointer alongside `m_fb[]`. |
| `QEtnaviv2dBackingStore` | In SW mode: hold a plain `QImage` as the paint target. In `flush()`: `memcpy` dirty rect into the scanout BO mmap; skip `etna2d_blit` / `etna2d_finish`. |
| `QEtnaviv2dIntegration` | If SW mode: skip `etna2d_device_open` + `setGlobalContext`; construct screen in a no-GPU state. |

**Trigger:** `QT_ETNAVIV2D_SW=1` env var (or equivalently `render=none` QPA
arg).  No new files.  `libetna2d`, the blitter, and the atomic KMS path are
untouched.

**What the comparison measures:**

```
our SW mode   =  CPU paint  +  memcpy  +  vblank wait
our GPU mode  =  GPU paint  +  etna2d_blit  +  etna2d_finish  +  vblank wait
linuxfb       =  CPU paint  (direct fb0 mmap write, no copy, no vblank)
```

The `memcpy` (width × height × 4 bytes) is the one overhead SW mode adds over
linuxfb; it can be timed independently to isolate the display-step cost.

**Not a regression risk:** SW mode is an opt-in path; the default GPU path and
all existing tests are unaffected.

---

## Deliverables

- `qpa/src/qetnaviv2dscreen.{h,cpp}` — atomic KMS, plane enumeration,
  plane-target selection, overlay ARGB8888/`SRC_OVER` config,
  `attachPrimaryFb()` staging slot, 3-deep primary ring, shared flip-event
  release.
- `qpa/src/qetnaviv2dintegration.cpp` — `plane=`/`card=` parsing + precedence.
- `compositor/test_planes/` — split-plane video+UI test (single-owner).
- `compositor/plan.md` — this document.
- `qpa/qt-test.md` — add a `test_planes` section (plane map, run rungs, pass
  criteria) mirroring `test_kms_thread`.

---

## Scope: v1 vs v2

### v1 (this plan — acceptable milestone)

- Atomic KMS conversion; legacy kept as build-time A/B oracle.
- `plane=` argument: `primary`, `overlay`, `wayland` (handoff stub), `auto`.
- **UI → full-screen overlay**, premultiplied `SRC_OVER`, over primary.
- Single-owner model: Qt process owns both planes in **one atomic commit**.
- Video via **software decode + CPU memcpy** into a **3-deep** NV12 primary
  ring; `attachPrimaryFb()` latched staging.
- `plane=overlay` standalone over background color.
- `test_planes` covering the three rungs (primary regression / overlay-blend /
  split-plane video).

### v2 (deferred — implement last, in this order)

1. **Zero-copy dmabuf video import** — GStreamer dmabuf `appsink` →
   `drmModeAddFB2WithModifiers`, no CPU copy. **Gated on the VPU/KMS driver
   being ready** (D2). Highest-value v2 item.
2. **Sub-rectangle overlay** — `--ui-rect=WxH+X+Y` driving both overlay
   position and Qt screen geometry; UI as a positioned panel, video
   full-screen behind (D5).
3. **Wayland transport + Weston multi-process** — adapt `QEtnaviv2dScreen`'s
   transport layer to present via `wl_surface` + `linux-dmabuf-v1` instead of
   direct KMS. `EtnaBlittable` and `libetna2d` are **unchanged** — GC520 GPU
   ops are preserved. Qt and GStreamer become Wayland clients; Weston owns
   plane assignment (Phase 3, D11). The `wayland` handoff stub is the seam.

Explicit **non-goals** (not v1, not v2): DRM-lease multi-process (two raw-KMS
processes on one card) — superseded by the Wayland compositor path (D4).

---

## Decisions log (from the /grilling session)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Single-owner model for the test program. | No IPC; one atomic commit = perfectly synced planes; de-risks the plane path. |
| D2 | **(b)** Software decode + CPU memcpy into recycled BOs. Zero-copy dmabuf = v2. Pre-decoded raw frames acceptable if perf poor. | VPU/KMS driver not yet ready; can't dmabuf-import. |
| D3 | **(a)** QPA owns the single atomic commit; video path attaches primary FB via a hook. | Keeps one committer; preserves `m_flipMutex` (Risk #5); single-vblank sync. |
| D4 | **Cut** the DRM-lease multi-process path. Multi-process = Wayland only. | DRM leasing (`DRM_IOCTL_MODE_CREATE_LEASE`, supported in 6.6) is the correct kernel mechanism for per-process plane ownership — the master carves out objects and passes a leased fd whose commits pass the `_drm_lease_held()` check. But leasing is what a Wayland compositor *is*: it holds master, leases (or directly commits) planes per client. Building our own lease protocol duplicates that work and couples us to raw KMS forever. Two raw-KMS processes fighting over the card is exactly what a compositor solves. |
| D5 | **(a)** Full-screen overlay for the UI. Sub-rect = v2. | QPA already owns a full screen-sized surface; smaller means teaching Qt reduced geometry. |
| D6 | **(a)** `plane=overlay` runnable standalone over background color. | Makes the blend path testable in isolation between Steps 2 and 4. |
| D7 | **(a)** Build-time `#ifdef` keeps legacy as a frozen A/B oracle; atomic is default/only plane path. Display driver owned in-house → atomic always available. | Regression comparison without maintaining two feature-complete paths. |
| D8 | **(b)** Blend correctness confirmed from the MA35D1 display spec (premultiplied `SRC_OVER`, standard formula). No conversion. Hardware halo check kept as a wiring-regression guard. | Spec verified by the driver owner; matches Qt premultiplied output. |
| D9 | **(a)** `attachPrimaryFb()` = latched, mutex-protected staging slot; last-write-wins; producer never blocks on vblank. | Reuses existing serialization; decouples GStreamer thread from display timing. |
| D9b | Primary uses **3 buffers, each with one `on_screen` bit** (cleared on the flip event). The producer picks only a buffer that is neither `on_screen` nor `staged`. | A depth-only "ring" (or a 2-BO swap) has no ownership tracking: a producer running >1 frame ahead within a vblank overwrites the buffer the DC is scanning → tearing / torn NV12. One bit per buffer, cleared on the existing flip event, is minimal and race-free. |
| D10 | **(a)** Shared single-event release; overlay **2-deep**, primary **3 buffers + on_screen bit** (D9b). | One commit → one flip event → the overlay flips and the committed primary buffer is freed together. Asymmetric because producers differ (inline Qt vs. decoupled video). |
| D11 | Under Weston, the etnaviv2d QPA is **adapted, not replaced**: only `QEtnaviv2dScreen`'s transport switches from direct KMS to `wl_surface` + `linux-dmabuf-v1`. `EtnaBlittable`/`libetna2d` are unchanged. | The custom QPA is the sole holder of GC520 DE knowledge; the stock Wayland QPA has none. Retiring it would drop GPU acceleration under a compositor. Weston owns arbitration, not rendering. |
| D12 | The video producer is a **first-class present trigger**: `attachPrimaryFb()` drives a coalesced GUI-thread present (`QWindow::requestUpdate()`) after staging. Coalesced to ≤1/vblank; skipped if a flip is already pending. | With a single committer, commits must be triggerable by *either* producer. Symmetric UI/video triggering means a static UI never stalls the video. Maps to a `wl_surface` frame callback under Weston. |

### Risks — final status

| Risk | Status |
|------|--------|
| R1 — premultiplied vs straight alpha on overlay | **Closed by spec** (D8). Premultiplied `SRC_OVER`, no conversion. Halo check as wiring guard. |
| R2 — immutable zpos / layer order | **Closed** (DT confirmed: overlay zpos 1 > primary zpos 0). |
| R3 — atomic vs legacy regression | **Mitigated** — legacy kept as build-time oracle (D7); re-run full QPA suite after Step 1. |
| R4 — NV12 plane support | **Sufficient for v1** — `MA35_FORMAT_NV12` in plane format list. Zero-copy dmabuf import is v2. |
| R5 — video stalls under a static UI | **Not a risk — by design** (D12). The video producer is a first-class present trigger via a coalesced GUI-thread present, symmetric with UI-driven commits. Pass criterion in Step 4. |
| R6 — producer overwrites on-screen primary buffer | **Closed by design** (D9b) — 3 buffers + `on_screen` bit; producer never picks the scanned-out or staged buffer. Pass criterion: fast decode with a static UI shows no tearing on the primary. |
