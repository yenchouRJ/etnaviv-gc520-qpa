# IVI-shell - `waylandsink` IVI surface-role support ✓ DONE

**Goal:** Let GStreamer `waylandsink` present video under **ivi-shell**
(instead of only kiosk-shell/`xdg_shell`), so video gets the same
client-assigned, launch-order-independent, ID-driven plane routing that the
Qt UI (via the QPA's own Wayland transport) already has.

> **Status (2026-09-15): DONE.** `waylandsink` patched, built, deployed, and
> hardware-verified under ivi-shell (correct plane/zpos routing,
> launch-order independence, ID swap) with a clean kiosk-shell regression.
> Runtime plane swap (no client restart) also implemented and
> hardware-verified - see **Runtime plane swap** below. The only local
> product code touched is `ivi/nu-hmi-controller/nu-hmi-controller.c`; the
> `waylandsink` deliverable remains a GStreamer patch only.

---

## Background

ivi-shell + `nu-hmi-controller` (the id → layer → plane routing controller)
were implemented and hardware-verified earlier (see `ivi/deferred.md` for
that original investigation), but were blocked as a *product* deliverable
because `waylandsink` always created a plain `xdg_toplevel` internally and
exposed no way to bind its `wl_surface` as an `ivi_surface`. This plan closes
that gap.

`waylandsink`'s Wayland integration is not self-contained in
`ext/wayland/gstwaylandsink.c` - it's built on a shared library,
`gst-libs/gst/wayland/`, which already handles arbitrary pixel formats and
dmabuf import/export generically. Only **surface-role creation** (`xdg_wm_base`
→ `xdg_surface` → `xdg_toplevel`) is hardcoded to `xdg_shell`. That is the
only thing that needed to change - no new sink, no format/buffer-pool work.

---

## What changed - `gst1-plugins-bad-1.22.9` patch

No local product code was touched. The entire deliverable is one GStreamer
patch (`gst-libs/gst/wayland/`), committed in the nested git repo at
`output/build/gst1-plugins-bad-1.22.9` (baseline `d3c262a`, patch `a67c2b5`),
exported as `package/gstreamer1/gst1-plugins-bad/0002-waylandsink-ivi-application-support.patch`.

| File | Change | Purpose |
|---|---|---|
| `gstwldisplay.h` | + `ivi_application` field | Storage for the bound `struct ivi_application *` proxy. |
| `gstwldisplay.c` | Bind `ivi_application` in the registry listener alongside the existing `xdg_wm_base` bind; new `gst_wl_display_get_ivi_application()` accessor (mirrors `gst_wl_display_get_xdg_wm_base()`). | Detect whether the compositor is ivi-shell (both `ivi_application` and `xdg_wm_base` are advertised) vs. kiosk-shell (`xdg_wm_base` only). |
| `gstwlwindow.c` | In `gst_wl_window_new_toplevel()`: if `ivi_application` is bound, call `ivi_application_surface_create(ivi_app, ivi_surface_id, area_surface)` instead of the `xdg_wm_base_get_xdg_surface`/`xdg_surface_get_toplevel` sequence. `ivi_surface_id` read from `GST_WAYLANDSINK_IVI_SURFACE_ID` env var (default `1000`). Falls through unchanged to the existing `xdg_wm_base` path when `ivi_application` is not bound. | The actual IVI surface-role branch - mirrors the existing pattern already used by the Qt QPA transport (`QT_IVI_SURFACE_ID`, default `2000`). |
| `gstwlwindow.c` | `gst_wl_window_ensure_fullscreen()` now a no-op when `xdg_toplevel == NULL`. | ivi-shell has no `xdg_toplevel`-style fullscreen concept; avoids a NULL deref. |
| `gstwlwindow.c` | `gst_wl_window_is_toplevel()` returns `TRUE` for `ivi_surface != NULL` too. | Keeps `gst_wayland_sink_change_state()`'s PAUSED→READY own-vs-shared-window logic correct under ivi-shell. |
| `gstwlwindow.c` | Do **not** block waiting for `ivi_surface.configure`; mark `configured = TRUE` immediately after the initial commit (see bug #2 below). | ivi-shell's configure round trip (100–190ms measured) is far slower than `xdg_surface`'s (near-instant); blocking stalled the first render enough to wedge small fixed-size upstream ring buffers at 30fps. |
| `meson.build` | Vendor `protocol/ivi-application.xml` (copied from `weston-10.0.5/protocol/`, a Weston/AGL extension, not upstream `wayland-protocols`); generate its client bindings via `wl_scanner` the same way as the other protocols. | `ivi_application`/`ivi_surface` are not in the standard protocol set shipped with `wayland-protocols`; the XML must be vendored. |

New env var, mirroring the existing QPA convention:

| Env var | Default | Read by |
|---|---|---|
| `GST_WAYLANDSINK_IVI_SURFACE_ID` | `1000` | `gstwlwindow.c`, `gst_wl_window_new_toplevel()` (no GObject property - inert under kiosk-shell, since `ivi_application` is never bound there). |

Layer IDs `1000` (video/primary, `plane[31]`, zpos=0) and `2000` (Qt UI/overlay,
`plane[38]`, zpos=1) in `weston-ivi.ini` `[nuvoton-layer]` sections are
finalized as permanent product values - both sink defaults are keyed off them.
No changes were needed to `nu-hmi-controller.c` or `weston-ivi.ini`.

---

## Bugs found during this dev journey

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 1 | Nothing rendered on first attempt - build succeeded but `ivi_application` never bound | `meson.build` generated bindings for the vendored XML into the wrong scanner target; not actually a runtime bug, caught before verification (build-time only) - no code symptom to chase, resolved by matching the existing protocol-generation pattern in `meson.build` exactly. | Vendor `ivi-application.xml` + generate client bindings identically to the other protocols already in `meson.build`. |
| 2 | **fps=30 stall under ivi-shell only** (`gst_dmabuf_video`: "ring stall - all slots busy"); fps=10 worked fine; kiosk-shell worked fine at fps=30 | `ivi_surface.configure` takes ~100–190ms round-trip under ivi-shell (vs. near-instant for `xdg_surface.configure`). The original patch reused the xdg path's blocking-wait-with-100ms-timeout, which was juuust too short. Window creation stalled the first `show_frame()` call long enough for 2–3 buffers to pile up upstream and get silently dropped by `waylandsink`'s own `redraw_pending` QoS gate (pre-existing, unrelated to this patch) - which wedged the test harness's fixed 3-slot round-robin ring permanently, since the one buffer that *did* render was never replaced/released. | Stop blocking on `ivi_surface.configure` entirely; mark `configured = TRUE` immediately after the commit - same "eager configure" pattern the QPA transport (`qetnaviv2dwaylandtransport.cpp`) already uses for ivi-shell. The real configure event, whenever it arrives, is now a harmless no-op. |

Diagnosed via `GST_DEBUG=waylandsink:6,wlwindow:6,wldisplay:6` traces
comparing kiosk-shell vs. ivi-shell timing at fps=30 and fps=10.

---

## Verification (hardware-verified, `192.168.0.102`)

| Test | Result |
|---|---|
| ivi-shell, video alone (`GST_WAYLANDSINK_IVI_SURFACE_ID=1000`) | `plane[31]` zpos=0, NV12, `PLANES_ONLY`. |
| ivi-shell, video (1000) + Qt UI (2000) concurrently, video launched first | Correct routing regardless of launch order - video primary/zpos=0, UI overlay/zpos=1. |
| ID swap test (`GST_WAYLANDSINK_IVI_SURFACE_ID=2000` + `QT_IVI_SURFACE_ID=1000`) | Planes swap correctly - whichever client owns ID `1000` always lands on primary/zpos=0. |
| ivi-shell, fps=30 (post-fix) | Smooth, no ring stalls (`pushed 30/60/90/120 frames` at ~1000ms/30-frame cadence). |
| kiosk-shell regression, fps=30 | Unchanged `xdg_toplevel` path taken (no `ivi_application` in registry); smooth, identical to pre-patch Milestone A/C behavior; no dmesg warnings. |

---

## Why a custom `nu-hmi-controller.so` instead of stock `hmi-controller.so`

Weston ships a reference HMI controller,
`weston-10.0.5/ivi-shell/hmi-controller.c` (~2000 lines), built for the
AGL/GENIVI reference "home screen" use case: base/application/workspace/
launcher layers, tiling/side-by-side/fullscreen/random-replace window modes,
launcher icons, fade animations, and its own custom Wayland protocol
extension for an external shell-UI client. None of this applies to an
embedded kiosk with exactly two fixed clients (video + Qt UI) and no
launcher/home-screen concept.

The one behavior actually needed - and the entire reason to use ivi-shell at
all - is: **a client-assigned, immutable IVI surface ID that deterministically
maps to a specific layer (and therefore a specific hardware plane/zpos),
independent of launch order or restart timing.** This is the opposite of
kiosk-shell's implicit heuristic (plane assignment follows Weston's natural
view-stacking/creation order), which works for a fixed two-client kiosk but
can't guarantee a client restarting independently (e.g. after a crash) lands
back on *its* plane rather than whichever plane is currently on top.

`ivi/nu-hmi-controller/nu-hmi-controller.c` (~340 lines) implements exactly
that mapping and nothing else:

- Reads `[nuvoton-layer]` (`id` + `render-order`) sections from `weston.ini`
  at startup; creates one `ivi_layout_layer` per section - no
  base/workspace/launcher layers.
- On `configure_surface` notification, looks up the layer whose `id` matches
  the surface's IVI ID (`get_id_of_surface()`) and adds the surface to it -
  full-output source/destination rectangle, visible.
- Surfaces with no matching ID (or `xdg_toplevel` desktop surfaces under
  ivi-shell) go to a catch-all `id=9999` overflow layer, kept at the bottom.
- No custom Wayland protocol, no animations, no launcher.

This was already implemented and hardware-verified before the `waylandsink`
patch work in this plan (see `ivi/deferred.md`); no changes to
`nu-hmi-controller.c` or `weston-ivi.ini` were needed here - routing keys
purely off the IVI surface ID, the same regardless of which kind of client
presents it (bare `test_weston` harness, patched `waylandsink`, or the Qt
QPA transport).

---

## `test_weston --ivi-id`'s role, going forward

Diagnostic/compositor-level only (its original Phase 6 purpose - proving the
DRM-backend dmabuf → hardware-plane path with a bare C client). Not part of
the shipped IVI video pipeline; patched `waylandsink` is. Kept in-tree as a
low-level debugging tool for `nu-hmi-controller`/DRM-backend issues that
don't require a full GStreamer pipeline to reproduce.

---

## Future: real VPU decode - zero sink-side work required

The planned VC8000 driver replacement is DRM-based (not V4L2), paired with
a new custom GStreamer decode *element* - not a new sink.
Pipeline becomes `filesrc ! h264parse ! <custom VPU DRM decode element> !
waylandsink (patched)`. The only requirement on that future element: produce
dmabuf-backed `GstBuffer`s in a format `gstwlvideoformat.c` already knows
about (RGB or YUV/NV12 family both covered). No further sink-side change is
anticipated - this patch and the env-var ID selection are orthogonal to the
decode backend. `gst_dmabuf_video`'s hand-rolled appsrc/dumb-BO-ring becomes
unnecessary once the VPU element exists and can be retired then.

---

## Relevant files

- `output/build/gst1-plugins-bad-1.22.9/gst-libs/gst/wayland/gstwldisplay.{c,h}` - `ivi_application` bind + accessor.
- `output/build/gst1-plugins-bad-1.22.9/gst-libs/gst/wayland/gstwlwindow.c` - `ivi_surface` role branch, eager-configure fix.
- `output/build/gst1-plugins-bad-1.22.9/gst-libs/gst/wayland/meson.build` - vendored protocol generation.
- `output/build/gst1-plugins-bad-1.22.9/gst-libs/gst/wayland/protocol/ivi-application.xml` - vendored protocol XML.
- `package/gstreamer1/gst1-plugins-bad/0002-waylandsink-ivi-application-support.patch` - generated via `git format-patch`, verified byte-identical to the source commit.
- `package/weston/weston-ivi.ini` - `[nuvoton-layer]` IDs `1000`/`2000`, finalized as permanent.
- `wayland/tests/gst_dmabuf_video/gst_dmabuf_video.c` - verification harness; header comment updated to document ivi-shell support and the `SIGUSR1`-to-weston swap trigger.
- `qpa/src/qetnaviv2dwaylandtransport.cpp:116-127` - reference implementation of the env-var + eager-configure pattern mirrored by this patch.
- `ivi/nu-hmi-controller/nu-hmi-controller.c` - **modified** for runtime plane swap (`current_surface` tracking, `on_swap_signal()`, `SIGUSR1` registration); layer routing logic for initial assignment unchanged. See "Why a custom `nu-hmi-controller.so`" above and "Runtime plane swap" below.
- `ivi/deferred.md` - original ivi-shell + `nu-hmi-controller` investigation this plan resumed from.

---

## Decision log

| ID | Decision | Rationale |
|----|----------|-----------|
| IVI-patch-scope | Patch the shared `gst-libs/gst/wayland/` library, not a bespoke sink or a new GStreamer element. | Format/buffer-pool machinery (`gstwlvideoformat.c`, `gstwllinuxdmabuf.c`, `gstwlvideobufferpool.c`) is already generic and format-agnostic; only surface-role creation was `xdg_shell`-specific. Writing a new sink would duplicate work that already exists. |
| IVI-id-selection | Env var (`GST_WAYLANDSINK_IVI_SURFACE_ID`), not a GObject property. | Mirrors the existing `QT_IVI_SURFACE_ID` convention in the QPA transport; no new pipeline-visible API surface; inert under kiosk-shell. |
| IVI-configure | Eager-configure (mark configured immediately after commit) instead of blocking on `ivi_surface.configure`. | ivi-shell's configure round trip is measurably slower (100-190ms) than xdg's; blocking caused a real 30fps stall (bug #2). Matches the pattern the QPA transport already uses for the same protocol behavior. |
| IVI-hmi-controller | Keep the existing minimal `nu-hmi-controller.so` rather than adapting Weston's stock reference `hmi-controller.so`. | Stock controller is a ~2000-line AGL/GENIVI home-screen reference (launcher, animations, custom protocol) with no product value for a fixed two-client kiosk; the only needed behavior (ID → layer → plane routing) is already fully covered by the existing 340-line controller. |
| IVI-swap-location | Runtime plane swap implemented compositor-side (`nu-hmi-controller.c` swaps its layer↔surface routing table), not client-side (`waylandsink`/QPA destroy+recreate `ivi_surface`). | Client-side reassignment was implemented and hardware-tested first, then reverted: any two-client id-swap timing where one client claims an id the other hasn't released yet triggers a *fatal* `wl_resource_post_error` that kills the whole client connection (measured on hardware) - there is no synchronous "id free?" check in `ivi_application` to avoid this race. Compositor-side swap has no client-visible protocol interaction at all, so no race exists. |

---

## Runtime plane swap (no client restart) ✓ DONE

**Goal:** let an operator swap which client (video vs. Qt UI) occupies the
primary/overlay plane while both processes keep running - no stop/relaunch,
no dropped frames, no interruption to either GStreamer pipeline.

### Why not client-side (rejected, hardware-tested)

The obvious approach - have each client destroy its `ivi_surface` and
recreate it with the other's id (`gst_wl_window_toggle_ivi_surface()` in
`gstwlwindow.c`, plus a `waylandsink` `"toggle-ivi-surface"` action signal,
triggered by `SIGUSR1` to each client process) - was implemented and hardware
-tested, then reverted. It is legal per the `ivi_application` protocol in
isolation (`weston_surface_set_role()` only checks the role *name*, and
`ivi_layout_surface_create()` only rejects an id that's *currently* in use -
confirmed by reading `weston-10.0.5/ivi-shell/ivi-shell.c` /
`ivi-layout.c`), but **fails fatally under real two-client swap timing**:

```
Client A (id=1000) destroys its ivi_surface, tries to create id=2000
  while Client B (id=2000) still holds it
    → Weston: wl_resource_post_error(IVI_APPLICATION_ERROR_IVI_ID,
      "surface_id is already assigned by another app")
    → per Wayland core semantics, a posted protocol error is FATAL to the
      *entire* client connection, not just that one request
    → measured on hardware: "libwayland: error in client communication
      (pid N)" - Client A's whole Wayland connection dies, its surface is
      already gone (destroyed before the failed create), and it can never
      recover without a full process restart - exactly the failure mode
      this feature was meant to eliminate.
```

There is no synchronous "is this id free?" query in `ivi_application`, so
there is no way to avoid the race client-side - even a "park on a scratch id
first" retry still races the *final* claim against whatever the other client
is doing. **Client-side id reassignment cannot be made race-free with the
standard `ivi_application` protocol alone.**

### What was built instead: compositor-mediated swap

Neither client's `ivi_surface` id, `wl_surface`, or Wayland connection is
touched at all. `nu-hmi-controller.c` (which already owns both surfaces'
`ivi_layout_layer`/`ivi_layout_surface` objects) swaps its own
**layer↔surface routing table** in a single, already-serialized,
compositor-side operation - no cross-process race window exists because
nothing client-visible changes.

| Piece | Where | What |
|---|---|---|
| `struct nuvoton_layer.current_surface` | `nu-hmi-controller.c` | Tracks which `ivi_layout_surface` currently occupies each configured layer (set in `assign_surface_to_layer()`). |
| `on_swap_signal()` | `nu-hmi-controller.c` | `layer_remove_surface()` + `layer_add_surface()` to swap the surfaces on `layers[0]` and `layers[1]` (the two lowest-`render-order` layers - `1000`/`2000` in this product); reapplies full-output source/destination rects via the extracted `set_surface_geometry_fullscreen()` helper; `commit_changes()`. |
| Trigger | `wet_module_init()`, `nu-hmi-controller.c` | `wl_event_loop_add_signal(wl_display_get_event_loop(ec->wl_display), SIGUSR1, on_swap_signal, hmi)` - libweston's own event-loop-integrated signal handling (signalfd-based), safe to call `ivi_layout` API from directly, unlike a raw `sigaction` handler. |

Operator usage: `kill -USR1 <weston pid>`. No client-side code, no
`waylandsink`/QPA changes of any kind.

Usage example:
```cpp
#include <csignal>
#include <unistd.h>

kill(weston_pid, SIGUSR1);   // triggers on_swap_signal() in nu-hmi-controller
```

### Verification (hardware-verified, `192.168.0.102`)

| Test | Result |
|---|---|
| Baseline | `plane[31]` NV12 zpos=0 (video, id=1000), `plane[38]` AR24 zpos=1 (Qt UI, id=2000). |
| `kill -USR1 <weston pid>` | `plane[31]` XR24 zpos=0 (Qt UI), `plane[38]` NV12 zpos=1 (video) - swapped. Both processes stayed alive; `gst_dmabuf_video`'s frame counter never paused or reset across the swap (continuous `pushed N frames` log, no ring stalls). |
| Second `kill -USR1` | Swapped back to baseline - repeatable, idempotent toggle. |
| kiosk-shell | Unaffected - `nu-hmi-controller.so` (and therefore `SIGUSR1` handling) is only loaded via `weston-ivi.ini`; `weston-kiosk.ini` never loads it. |

### Limitation

`on_swap_signal()` always swaps exactly the two lowest-`render-order`
layers (`layers[0]`/`layers[1]`). Fine for this product's fixed two-client
model; would need a real selection mechanism (which two layers to swap) to
generalize beyond two configured `[nuvoton-layer]` entries.

---

## Next steps

All items below are done; kept here as a record.

1. ✓ Generated `package/gstreamer1/gst1-plugins-bad/0002-waylandsink-ivi-application-support.patch` via `git format-patch` from the `output/build/gst1-plugins-bad-1.22.9` git repo. Verified byte-identical to a fresh `git format-patch -1 a67c2b5` output.
2. ✓ Updated `gst_dmabuf_video.c`'s header comment to remove the stale IVI-limitation note (it now works via the sink patch, not by calling `ivi_application_surface_create()` itself) - header now documents both ivi-shell support and the `SIGUSR1`-to-weston swap trigger.
3. ✓ `nu-hmi-controller.c` is tracked directly in this repo, not vendored from an upstream git tree - no `package/` patch needed. Header comment already documents the runtime-swap behavior added in this plan.

Note: `weston-10.0.5` itself also received unrelated DRM/pixman fixes during this
work (KMS plane format advertisement, hw cursor-plane allocation) that predate
and are independent of this plan's IVI-shell goal. Generating buildroot patches
for those is out of scope here - not tracked as a next step in this plan.
