# etnaviv2d QPA: improvement plan (verify step by step)

Based on the design review of `plan.md`, `qpa/src`, `hal/libetna2d`. Each step has
a hypothesis, a change, a test and a pass criterion. Do them in order; do not start
step N+1 until step N is measured and committed. Every step must keep
`qt2dbench --matrix` bit-exact (the regression gate).

## Test tools
- `qpa/tests/qt2dbench/` (copied from graphics_presentation/assets/gpu/tests/qt2dbench):
  `--matrix` (correctness, 42 cases), `--ops`, `--clip`, `--sprites N`, `--stage-secs S`, `--display`.
- `qpa/tests/test_blitter`, `test_benchmark`, `test_glyph` (text), `test_kms_thread`, `test_screen`, `test_slideshow`.
- Env: `QT_ETNAVIV2D_DEBUG=1`, `QT_ETNAVIV2D_NO_BATCH=1`, `QT_ETNA2D_FMT`.
- Run: `QT_QPA_PLATFORM=etnaviv2d:plane=primary` (stop weston first) or `plane=wayland QT_IVI_SURFACE_ID=2000`.
- Target: `ssh root@192.168.0.102`; `modetest -M ma35-drm`. (`/mnt/vivante2d/qpa` and
  `/mnt/graphics_stack` were not mounted when checked: mount/copy them before testing.)
- Underflow check: clear `devmem 0x40261518` / `0x40261540`, poll, bit 5 = primary, bit 23 = overlay.

## Step 0: Baseline and instrumentation (no behaviour change)
- Add per-frame counters in the plugin, printed when `QT_ETNAVIV2D_DEBUG=2`:
  time in `lock()` waits, `doUnlock()`/`etna2d_finish()` count and time, CPU fallback time
  (`cpuFillRect`/`cpuDrawPixmap`/raster on locked image), `flush()` copy time, `pageFlip` wait time.
- Record baseline on target: `--matrix`, `--ops`, `--clip --sprites 16`, `test_glyph`, `test_benchmark`
  at 1024x600 and 1920x1080, for ARGB8888 and rgb565. Save to `qpa/results/step0-*.txt`.
- Pass: numbers reproducible (3 runs within 5%). The breakdown shows which of steps 1-4 matters most.

### Step 0 results (done; Wayland ivi, weston 10, kernel 6.6.93, 3 runs each, spread < 1%)
Counters: `QT_ETNAVIV2D_DEBUG=2` prints one `ETNASTATS` line/s (`src/qetnaviv2dstats.h`). Script: `tests/qt2dbench/run_baseline.sh`, raw output in `results/step0-*.txt`.

| `--clip --sprites 16` | fps | ms/frame | flush finish ms per frame | copy MB/s |
|---|---|---|---|---|
| 1024x600 argb8888 | 70.5 | 14.2 | 11.2 (flushfin 683 ms/s over 61 flushes) | 143 |
| 1024x600 rgb565 | 121 | 8.3 | n/a (see raw) | |
| 1920x1080 argb8888 | 28.3 | 35.4 | 30.9 (897 ms/s over 29 flushes) | 229 |
| 1920x1080 rgb565 | 67 | 14.9 | 12 | 237 |

`--ops` (offscreen, 1024x600, argb8888): fill full 211 Mpix/s, blit/alpha full 71, scale full 169, **alphafill 14-17 Mpix/s at every size** (256x256: 4.6 ms/op; it is the CPU raster on WC memory, ~100% of one core). 64x64 ops cost 77-294 us each (a QPixmap round trip, dominated by lock/finish, not the 3 us GPU cost).
`--matrix`: 40 match, 2 DIFF (the known Qt 5.15 ellipse-clip limitation, section 6 of plan.md). This is the regression gate.

Findings that reorder the plan:
1. **`flush()` GPU time dominates**: 79% of the frame at 1024x600 and 87% at 1080p is `etna2d_finish()` after the full-frame blit plus the frame's queued ops. The whole-frame copy (Step 3) is therefore the first thing to attack, not Step 1.
2. rgb565 gives 1.7x/2.4x: the cost scales with bytes, i.e. the frame is memory-bandwidth bound, which confirms Step 3.
3. `lockfin` / `unlockfin`: 72 per second at ~1 ms each (about 7% of the frame): Step 2, smaller than expected in this scene because it has no CPU fallback (cpufill/cpupix = 0). The CPU fallback hot spot shows up only in `--ops alphafill` (Step 1).
4. 14% frames dropped at 1024x600 (ring full, `dropped=10-14` per s): back-pressure drop with no re-arm (Step 4). Not seen at 1080p argb (GPU-bound: Weston keeps up).
5. Build note: `hal/libetna2d` and `qpa` Makefiles default to the `ma35d1_br24_qt5` sysroot; with the graphic tree use `make -C hal/libetna2d BR=<graphic>` and `make plugin QT_BUILD=<graphic qt5base> BR_ROOT=<graphic>` (linking against the qt5 sysroot fails on /lib64/libm).

Proposed order after this: **Step 3 -> Step 2 -> Step 4 -> Step 1 -> 5 -> 6**.

## Step 1: CPU fallback memory type (WC -> cached)
Hypothesis: raster fallback reads the destination from WC memory (uncached, slow).
- libetna2d: `etna2d_surface_create_fmt` gets a flag/variant for `ETNA_BO_CACHED`
  (keep WC for scanout/ring). Add `etna_bo_cpu_prep/cpu_fini` in `etna2d_surface_cpu_fini` and a
  prep before CPU map. Check kernel etnaviv on MA35D1 handles cached BO sync (cache coherency on this SoC).
- Plugin: pixmaps and backing store use cached BOs.
- Test: `--matrix` bit-exact; `--ops` translucent fillRect and text (`test_glyph`) throughput.
- Pass: translucent fill >= linuxfb (31 Mpix/s at 1080p, was 13); no stale-pixel artifacts in a 10 min `test_slideshow` run.
- Fallback if kernel lacks cached sync: keep the backing store in malloc memory and upload (see step 3b).

## Step 2: Remove the full drain from `doUnlock()`
- Track a per-surface "GPU write pending" flag (submit counter/fence). `lock()` waits only if set.
  `doUnlock()` does `cpu_fini` only, no `etna2d_finish`. `isOpaque()` becomes a creation-time
  flag (alpha format and `hasAlpha` hint) or an incremental flag, no `lock()` scan.
- Single submit per frame in `flush()`.
- Test: `--matrix` exact; debug counters show `finish` count/frame drops to 1; `--clip --sprites 16` fps.
  `QT_ETNAVIV2D_NO_BATCH=1` must give the same pixels.
- Pass: finish count <= 1 per frame in a mixed text + sprite scene; fps up vs step 1; no hazards
  (CPU draw on a surface with a pending GPU write must still be correct: add a matrix case that alternates GPU fill / CPU text / GPU blit on one surface).

## Step 3: Damage-aware flush (remove the whole-frame copy)
- 3a: `flush()` blits only `region` rects. Per-ring-slot (or per-scanout-buffer) damage history:
  a slot must also repaint what changed since it was last used. Wayland: `wl_surface_damage_buffer` with the same region.
- 3b (if 3a still bandwidth bound): render straight into the ring/scanout BO (drop the offscreen copy).
- Test: a static UI with a small animated widget; `test_slideshow`; add a matrix check that compares
  screen readback (`toImage` of the ring) to the CPU reference after random partial updates over N frames (catches stale-slot ghosting).
- Pass: bytes copied per frame ~ dirty area; the underflow poll on primary with overlay 32 bpp goes 32/32 -> 0/32 without needing rgb565; no ghosting.

### Step 3 results (done; Wayland ivi path, KMS path implemented the same way but not run)
Change: `QEtnaviv2dBackingStore` keeps a stale region per target buffer (`takeDamage`): each flush adds its region to every known buffer, the target buffer receives exactly its stale region (rects, or one bounding blit when the cost model says it is cheaper) and becomes current; a new buffer or a resize is fully stale; a dropped frame (ring full) still accumulates. The Wayland commit damages the union of the regions since the last attach (including blitted-but-never-committed frames). `QT_ETNA2D_FULL_FLUSH=1` restores the old whole-frame behaviour (A/B).
Test tools: `qt2dbench --damage --sprites N` (static patterned background, N small moving sprites, only old/new rects `update()`d), `QT_ETNA2D_VERIFY_FLUSH=1` (after each blit the whole target buffer is compared with the backing store; `VERIFY FAIL` on mismatch), `QT_ETNA2D_TEST_DROP=N` (pretend the ring is full every Nth flush).

| `--damage --sprites 4`, 6 s, unpaced | full flush | damage flush |
|---|---|---|
| 1024x600: update loops/s | 184 | 275 |
| 1024x600: flushes/s, copy MB/s | 60, 141 | 85, 31 |
| 1920x1080: update loops/s | 61 | 269 (4.4x) |
| 1920x1080: flushes/s, copy MB/s | 31, 245 | 88, 34 |

- Correctness: `VERIFY FAIL` count 0 with 1 and 8 sprites and with `QT_ETNA2D_TEST_DROP=2` and `=3` (12 s each, hundreds of flushes). The verifier itself was not exercised with a deliberately broken tracker (negative test not done).
- Regression: `--matrix` 40 match / 2 DIFF (unchanged, known ellipse clip); `--ops` VERIFY lines ok; `--clip --sprites 16` unchanged (69.8 / 122 / 28.8 / 66 fps) because that app repaints the whole window every frame, so its damage is the whole frame. Gains need partial updates (typical widget UIs).
- The remaining unmet pass item: the primary-plane underflow poll with the overlay at 32 bpp has not been measured yet (needs a partial-update workload on the board with the poll, do it together with Step 4).
- New fact, `hal/libetna2d/opcost.c` (`results/opcost.txt`): a tiny blit costs 3.7-5 us per op once batched (as the plan assumed), 48x48 costs 31 us (copy) / 46 us (src-over), 256x256 0.63 / 0.96 ms, 1024x600 5.4 / 8.1 ms. The DE moves only ~100-190 Mpix/s, so bytes, not op count, set the cost: the bigger the copy the more damage tracking pays.
- Observed after Step 3: ring-full drops are now 40% of flushes at 1024x600 with this workload (58 dropped/s): the app outruns Weston's release; Step 4 (repaint on release) is what turns these into shown frames.

### Step 3 on the cockpit demo (real app: 1080p H.264 video on primary, Qt UI on overlay)
`run_cockpit.sh videos/cockpit_1080_30.h264`, kiosk-shell, `QT_ETNA2D_FMT=argb4444 QT2D_FPS=10 SYNC=false` (the script's 1080p defaults), 25 s clip, A/B with `QT_ETNA2D_FULL_FLUSH`. Raw: `results/step3-cockpit-{full,damage}.txt`.

| | full flush | damage flush |
|---|---|---|
| flush `etna2d_finish` ms per UI frame | 14.3 | 2.4-3.1 |
| UI copy MB/s (10 flushes/s) | 39.6 | 5-7 |
| video fps (gst fpsdisplaysink mean of 1 s samples) | 25.2 | 27.9 |

Video is the primary workload on the shared memory bus, so the UI copy saved shows up as +2.7 fps of video (30 fps source). One run each; repeat before quoting the fps gain as exact. Not checked visually for ghosting (only `VERIFY_FLUSH` on the synthetic test).

## Step 4: Non-blocking presentation
- KMS: `drmModePageFlip` without waiting in `flush()`; handle the flip event with a `QSocketNotifier` on the card fd; the next `flush()` while a flip is pending is deferred (single pending frame).
- Wayland: on ring-full, mark dirty and request a repaint from the `wl_buffer.release` handler instead of dropping. Use dma-buf fence/implicit sync instead of `etna2d_finish()` where the compositor supports it (verify on Weston).
- Test: `test_kms_thread`, `test_screen`; `--clip` at 60 fps pacing: frame time variance and dropped frames; stale-UI test (stop updating while ring is full, UI must end up current).
- Pass: no GUI-thread block on vblank (CPU time in `select` ~ 0); no stuck frame; no tearing.

### Step 4 results (Wayland part done; KMS part deferred)
Done: a frame dropped because all ring slots are held by Weston is no longer lost. The backing store already has the pixels and the dropped region was accumulated into every buffer's stale region (Step 3), so on the next `wl_buffer.release` the transport calls a hook that re-runs `flush()` with an empty region: the ring slot gets its stale region and the frame is committed. Before, nothing repainted until the app happened to draw again, so an idle UI could stay stale. (`QT_ETNA2D_NO_REPRESENT=1` restores the old behaviour for A/B.) Exit line `ETNAFINAL pending_drop=` must be 0.

| Test | Result |
|---|---|
| `--damage` with idle tail, `QT_ETNA2D_TEST_DROP=2` | new: `pending_drop=0`; with `QT_ETNA2D_NO_REPRESENT=1`: `pending_drop=1` (the stale-UI bug, reproduced) |
| same with `VERIFY_FLUSH`, drop 2 and 3 | 0 `VERIFY FAIL` |
| `--damage` natural drops (1024x600) | 55 dropped/s, 49 presented from the release hook |
| `--clip` 1024x600 rgb565 presented frames/s | 63 -> 95 (+50%); app paint fps 122 -> 104 (the app thread now also does the present work; unpaced stress case) |
| `--clip` 1024x600 argb8888 / 1080p | 69.8 -> 64.9 / 28.8 -> 28.2 fps (paint rate, same trade-off) |
| `--matrix` | 40 match / 2 DIFF (unchanged) |
| cockpit 1080p (A/B) | video 25.3 / 27.8 fps (unchanged vs Step 3; 10 fps UI never drops) |

Not done, with reasons:
- Replacing `etna2d_finish()` by a fence: the backing store is the source of the blit, and Qt repaints into it right after `flush()` returns, so the GPU must be finished (it is only 2-3 ms per frame after Step 3). Overlapping would need a double-buffered backing store. Not pursued.
- KMS non-blocking `pageFlip` + `QSocketNotifier`: not implemented, because the KMS path needs Weston stopped, has the composite/video modes and a mutex-held wait, and could not be A/B tested in this session. Do as its own change with a KMS harness (`test_kms_thread`, `test_screen`).
- `--clip` paint fps (an unpaced stress loop) goes down by 7-14% because dropped frames are now shown; if the app should rather run ahead, pace it (`QT2D_FPS`).

## Step 5: GPU use policy and text
- Size threshold: ops below N pixels (tune, expect ~16x16) stay on the CPU path. Measure per-op GPU cost (3 us) vs raster at several sizes with `--ops`.
- Evaluate a glyph path (`test_glyph` exists): A8 glyph mask is not a GC520 primitive; only pursue if the DE can do mask blit/colour expand (check `drivers/ref/GC520L`). Otherwise record "text is CPU" as the ceiling.
- Pass: no case slower than pure CPU raster by more than 5% in `--ops`.

## Step 6: Safety and hygiene
- Context/device per thread or mutex (scratch surface, `g_ctx` globals). Test: `test_kms_thread` + a QPainter on a worker thread pixmap.
- KMS-direct scanout format follows `QT_ETNA2D_FMT`; add ARGB4444 / ARGB1555 fill/blend matrix cases.
- Bind `wl_output`, size the ring/ivi surface from the toplevel (plan.md section 6.6); keep `QT_IVI_GEOMETRY` as override.

## Step 7 (optional, structural)
- Replace the hand-written Wayland client (`qetnaviv2dwaylandtransport.cpp`) with QtWayland plus a custom buffer integration. Only if steps 1-4 leave the transport as a maintenance or perf problem.

## Tracking
| Step | Status | fps / metric before | after | commit |
|---|---|---|---|---|
| 0 | done | baseline in results/ | counters added | see git log |
| 1 | todo | | | |
| 2 | todo | | | |
| 3 | done (Wayland) | 1080p partial update 61 -> 269 loops/s | copy 245 -> 34 MB/s | see git log |
| 4 | done (Wayland repaint-on-release; KMS deferred) | dropped frames lost | all presented, stale UI fixed | see git log |
| 5 | todo | | | |
| 6 | todo | | | |
