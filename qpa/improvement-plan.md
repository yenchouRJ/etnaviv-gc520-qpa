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

Proposed order after this: **Step 3 -> Step 2 -> Step 4 -> Step 1 -> 5 -> 6** (Step 5 data now makes Step 1 the next one).

## Step 1: CPU fallback memory type (WC -> cached)
Hypothesis: raster fallback reads the destination from WC memory (uncached, slow).
- libetna2d: `etna2d_surface_create_fmt` gets a flag/variant for `ETNA_BO_CACHED`
  (keep WC for scanout/ring). Add `etna_bo_cpu_prep/cpu_fini` in `etna2d_surface_cpu_fini` and a
  prep before CPU map. Check kernel etnaviv on MA35D1 handles cached BO sync (cache coherency on this SoC).
- Plugin: pixmaps and backing store use cached BOs.
- Test: `--matrix` bit-exact; `--ops` translucent fillRect and text (`test_glyph`) throughput.
- Pass: translucent fill >= linuxfb (31 Mpix/s at 1080p, was 13); no stale-pixel artifacts in a 10 min `test_slideshow` run.
- Fallback if kernel lacks cached sync: keep the backing store in malloc memory and upload (see step 3b).

### Step 1 results (done, but OPT-IN: `QT_ETNA2D_CACHED=1`; default stays write-combine)
**Decision**: visual A/B on real apps showed more display flicker with cached BOs (memory bandwidth limited system), so write-combine remains the default. The numbers below are for the opt-in path.
Change: `libetna2d` gets `etna2d_surface_create_ex(..., ETNA2D_SURF_CACHED)` (kernel `ETNA_BO_CACHED`: cacheable CPU map), `etna2d_surface_cpu_begin(ctx, s)` (submit queued ops, `etna_bo_cpu_prep(READ|WRITE)`: the kernel waits for the GPU fences of that surface only and invalidates the CPU cache) and `etna2d_surface_cpu_fini` (cleans the cache for the GPU). The plugin creates all its pixmaps and the backing store cached (`QT_ETNA2D_CACHED=0` = old write-combine for A/B); `doLock` uses cpu_begin instead of a whole-context `etna2d_finish`, `doUnlock` does cpu_fini and no finish. Ring and scan-out buffers (dma-buf imports) stay write-combine. Kernel already had the support (`etnaviv_gem_cpu_prep/fini` with `dma_sync_sgtable_*`); no kernel change.
Tests: `hal/libetna2d/cachedtest.c` (CPU write -> GPU blit -> CPU read, GPU fill over dirty cached lines -> CPU read, CPU read-modify-write -> GPU blit, 200 rounds of 1024x600): PASS; CPU read of one frame 109 ms on WC vs 4.8 ms cached. `--matrix` 40/2 unchanged; `--damage` + `VERIFY_FLUSH` bad=0; `etna2d_selftest` passes.

us/op (`run_ops_ab.sh`, 1024x600, plugin GPU path), WC -> cached (CPU raster reference in brackets):

| op | 64x64 | 256x256 | full |
|---|---|---|---|
| alphafill (always CPU) | 294 -> 103 (93) | 4609 -> 973 (870) | 35321 -> 7004 (5407) |
| fill / blit / alpha / scale | within +-5% | +3..+10% | fill 2723 -> 3222, blit 8343 -> 8805 |

Translucent fill is 5x faster and now within ~25-30% of a plain cached QImage; the remaining gap is the cache maintenance.
**Cost, to be fixed in Step 2**: every paint frame has one lock (QPainter creation locks the pixmap) and one unlock (first GPU op), and the kernel syncs the whole BO each time: measured at 1080p lockfin 5 -> 28 ms/s (about 1 ms per frame) plus the clean at unlock. GPU-only / partial-update workloads therefore lose a little: `--clip` 68 / 110 / 29 / 68 -> 62 / 104 / 27 / 60 fps (-5..-12%), `--damage` 1080p 282 -> 230 loops/s (-18%), 1024x600 275 -> 261 (-5%). CPU-fallback-heavy UIs (text, translucent fills, gradients, AA paths) gain; to get both, Step 2 should avoid the whole-BO sync when nothing needs it (e.g. keep the surface unlocked until a CPU op really starts, track whether the GPU wrote since the last sync).

## Step 2: Remove the full drain from `doUnlock()`
- Track a per-surface "GPU write pending" flag (submit counter/fence). `lock()` waits only if set.
  `doUnlock()` does `cpu_fini` only, no `etna2d_finish`. `isOpaque()` becomes a creation-time
  flag (alpha format and `hasAlpha` hint) or an incremental flag, no `lock()` scan.
- Single submit per frame in `flush()`.
- Test: `--matrix` exact; debug counters show `finish` count/frame drops to 1; `--clip --sprites 16` fps.
  `QT_ETNAVIV2D_NO_BATCH=1` must give the same pixels.
- Pass: finish count <= 1 per frame in a mixed text + sprite scene; fps up vs step 1; no hazards
  (CPU draw on a surface with a pending GPU write must still be correct: add a matrix case that alternates GPU fill / CPU text / GPU blit on one surface).

### Step 2 results (done, different shape than planned)
What was measured instead of assumed (`qt2dbench --mixed`: 12 big alpha blits queued on pixmap A, then a CPU-only antialiased-ellipse job on an unrelated pixmap B, repeated):
- The drain that hurts is not the unlock (`unlockfin` is 0 in every run) and not the lock of an unrelated surface: a CPU-only pixmap such as B stays locked across painters, so it never calls `doLock` again. The GPU work queued on A simply **sat unsubmitted in the command stream until A's own next lock**, which then waited the full 11 ms of GPU time (`A: ctor 11.2 ms`, whole loop 35.4 ms, the CPU idle while the GPU worked and the GPU idle while the CPU worked).
- Fix: `EtnaPaintEngine::end()` submits (`etna2d_flush`, asynchronous, no wait, no-op when nothing is queued) when a painter ends, so the GPU runs while the CPU continues. `QT_ETNA2D_SUBMIT_ON_END=0` = old behaviour.
- Result `--mixed`: 35.4 -> 29.3 ms per loop (-17%), A's painter 11.2 -> 0.2 ms; the CPU job gets slower (24 -> 29 ms) because GPU and CPU now share the DDR at the same time, the net is still a win. CPU busy 69 -> 100% of one core: the idle waiting is replaced by work.
- The per-surface kernel-fence wait at lock time (`etna2d_surface_cpu_begin` on write-combine surfaces, no `finish` at unlock) was implemented and measured: no measurable difference (`--mixed`, `--clip`, `--damage` within +-3%), so it was removed again; the WC lock stays `etna2d_finish` (free when nothing is queued). The per-surface "GPU write pending" flag from the plan is therefore not needed. `isOpaque()` is already cached per content version (`opaquescan` = 0).
- Regression check: `--matrix` 52 match / 2 known DIFF, `--damage` + `VERIFY_FLUSH` bad=0 (300 frames, full and partial flush), `--clip` unchanged (fps 89 / 31 same as before in the same session), `--ops` all within +-3% of step 5b.

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

### Step 5 results (policy measured; thresholds stay at 0; one libetna2d fix found on the way)
Tools: `QT_ETNA2D_MIN_FILL|BLIT|ALPHA|SCALE=<px>` / `QT_ETNA2D_MIN_ALL` (below this destination area the plugin draws with the CPU raster path, default 0 = always GPU); `tests/qt2dbench/run_ops_ab.sh` (GPU plugin vs the `offscreen` platform = CPU raster in ordinary cached memory, per op and size, offscreen, unpaced, us per op); `qt2dbench --ops` now also covers 8, 16, 32 px. Raw: `results/step5-*.txt`.

us/op at 1024x600 (GPU = plugin default; CPU cached = what a cached BO would give; plugin CPU on WC = `QT_ETNA2D_MIN_ALL` forcing the plugin's own CPU path, after the drain fix below):

| op | 8x8 gpu / cpu cached / plugin cpu(WC) | 64x64 | 256x256 | full |
|---|---|---|---|---|
| fill | 3.7 / 1.8 / 33 | 27 / 32 / 51 | 311 / 328 / 323 | 2723 / 1606 / 2677 |
| blit (opaque) | 4.7 / 2.2 / 52 | 71 / 33 / 520 | 931 / 559 / 7528 | 8343 / 4231 / 65706 |
| alpha (src-over) | 4.7 / 4.2 / 52 | 71 / 114 / 520 | 931 / 1387 / 7529 | 8589 / 9897 / 65720 |
| scale 2x | 5.0 / 3.2 / 46 | 33 / 41 / 143 | 394 / 598 / 1576 | 3373 / 3772 / 14637 |
| alphafill (always CPU today) | 6.2 / 4.1 / 6.3 | 294 / 93 / 294 | 4609 / 871 / 4610 | 35321 / 5409 / 35323 |

Findings:
1. **A size threshold does not pay today.** The plugin's CPU path (WC memory) is 8-90x slower than the GPU for every op except fill at 256x256+, so thresholds stay 0. Even with cached memory the CPU only wins for opaque blit (about 2x at all sizes: the DE moves ~100-190 Mpix/s), tiny fills and tiny ops (<= 8 px, where both are single-digit us); the GPU wins src-over (1.2-1.6x from 16 px up) and scale (1.2-1.5x from 32 px up). Revisit the knob after Step 1 (cached memory): the likely setting is opaque blit/fill to the CPU, alpha/scale on the GPU.
2. **Step 1 is now the biggest measured win**: the only always-CPU op, translucent fill, runs 4609 us (256x256) / 35 ms (full) on WC against 871 us / 5.4 ms on cached memory: 5-6.5x.
3. **Found and fixed in libetna2d: empty `etna2d_finish()` cost 118 us** (an empty submit plus fence wait) and every CPU fallback does two of them (`doLock` and `doUnlock`), about 240 us per fallback op (64x64 blit through the CPU path was 861 us, now 520; 8x8 fill 254 -> 33 us). `etna2d_finish()` now returns at once when nothing was queued since the last finish (`ctx->dirty`). Selftest passes, `--matrix` 40/2 unchanged, `--clip` unchanged (68 / 110 / 29 / 68 fps). `hal/libetna2d/emptyfinish.c` measures it (empty finish 118 -> 0 us; a real fill + finish stays 127 us, that is the submit round trip). This is the cheap part of Step 2; the per-surface fence part of Step 2 is still open. Deployed to `/usr/lib/libetna2d.so.1` (original saved as `/mnt/graphics_stack/gpu/libetna2d.so.1.orig`).
4. **Glyphs / opacity on the DE: not proven, not disproven.** The DE register header has `DE_FORMAT_A8`, `DE_FORMAT_MONOCHROME`, `VIVS_DE_SRC_CONFIG_MONO_TRANSPARENCY`, global source/dest alpha modes (`VIVS_DE_ALPHA_MODES_GLOBAL_*`, `GLOBAL_SRC_COLOR`), and the vendor feature database lists `G2D_MASK_AND_COLORKEY` and `2D_A8_TARGET/A8_NO_ALPHA` enums for GC520L, but nothing here proves A8 as a blend source, a coloured mask blit or global opacity works on this core. A text/opacity path needs a register-level probe first (A8 source + GLOBAL_SRC_COLOR blend; global alpha blit), as its own step (5b) with a pixel check against the CPU. Until then text stays CPU and benefits only from Step 1.

## Step 5b: global opacity and text on the DE (done)
**Probe** (`hal/libetna2d/alphaprobe.c`, brute force over the alpha/colour-multiply register fields, compared with the exact formula on random pixels, run on the board). Result: both work on GC520.
- **Global opacity**: premultiplied ARGB source, `ALPHA_MODES = SRC_BLEND ONE | DST_BLEND INVERSED | GLOBAL_SRC_ALPHA SCALED`, `COLOR_MULTIPLY = SRC_GLOBAL_PREMULTIPLY_ALPHA`, opacity in the top byte of `GLOBAL_SRC_COLOR`. Max error 2/255 on all four channels for any destination alpha. API `etna2d_blit_opacity()`.
- **Coloured A8 mask (glyph)**: `ETNA2D_FMT_A8` source, colour from `GLOBAL_SRC_COLOR` (straight ARGB, translucent colours work), same blend mode plus `COLOR_MULTIPLY = SRC_PREMULTIPLY | SRC_GLOBAL_PREMULTIPLY_ALPHA`. Max error 2/255 on rgb AND alpha for any destination alpha. API `etna2d_mask_blit()` and `etna2d_mask_blit_n()` (state emitted once, per glyph only SRC_ORIGIN + CLIP + DRAW_2D). Trap found by the probe: the cheaper "src blend NORMAL" variant is exact on an opaque destination but squares the alpha channel on a translucent one (text into an ARGB pixmap showed saturated colours).

**Plugin**
- `drawPixmapOpacity`: painter opacity < 1 with source-over, 1:1 goes to `etna2d_blit_opacity` (scaled / other composition modes stay CPU). `QT_ETNA2D_GPU_OPACITY=0` = old path.
- Text: `EtnaBlittable::drawCachedGlyphs` + `EtnaGlyphCache` (subclass of Qt's `QImageTextureGlyphCache`; the A8 atlas is mirrored into an A8 surface, dirty rectangles uploaded when glyphs were added). Composition modes other than SourceOver, mono and ARGB glyph formats, complex clips, rotated transforms: raster engine (CompositionMode_Plus draws from the same atlas on the CPU).
- `EtnaPaintEngine` (subclass of `QBlitterPaintEngine`, returned by `EtnaBlittablePlatformPixmap::paintEngine()`): the stock blitter engine locks the pixmap (a whole-context GPU drain) before every text run and again after the blittable's `drawCachedGlyphs`; with those two drains the GPU text was 2-3x SLOWER than the CPU (221 vs 69 us per 1-glyph op). Our engine calls the blittable directly for A8 / scale+translate / rect clip and never maps the surface. `QT_ETNA2D_GPU_TEXT=0` = old path.

**Results** (`run_ops_ab.sh`, us/op, write-combine, default; `results/step5b-*`):

| op | size | before | now | CPU raster ref |
|---|---|---|---|---|
| opacity (drawPixmap, opacity 0.5) | 64x64 | 544 | 72 | 132 |
| | 256x256 | 7915 | 941 | 1705 |
| | full | 66449 | 8668 | 11958 |
| text (16 px, dw/9 glyphs per op) | 1 glyph | 76.6 | 69.0 | 69.0 |
| | 14 glyphs (64) | 206 | 131 | 150 |
| | 113 glyphs (full) | 2473 | 1266 | 1102 |

All other ops unchanged (+-1%). `--clip` fps unchanged (68.6 / 109.6 / 28.7 / 67.1), `--damage` with `VERIFY_FLUSH` bad=0, `--matrix` 52 match / 2 DIFF (the two known ellipse-clip cases; 12 new text/opacity/atlas-growth/translucent-target cases all match, max diff <= 4).
Honest limit: Qt's own per-run work (layout, glyph position and cache lookups, about 60 us per `drawText` plus ~10 us per glyph) dominates small text, so the CPU is not freed for 8-16 px text, only for big glyphs, big opacity blits and translucent targets. The 2x speed-up over the write-combine CPU raster is the main gain.

## Step 6: Safety and hygiene
- Context/device per thread or mutex (scratch surface, `g_ctx` globals). Test: `test_kms_thread` + a QPainter on a worker thread pixmap.
- KMS-direct scanout format follows `QT_ETNA2D_FMT`; add ARGB4444 / ARGB1555 fill/blend matrix cases.
- Bind `wl_output`, size the ring/ivi surface from the toplevel (plan.md section 6.6); keep `QT_IVI_GEOMETRY` as override.

### Step 6 results (done)
- **Thread safety:** one GC520 context/queue is shared by all blittables, so a global recursive mutex (`etnaCtxMutex()`, `qetnaviv2dblitter.cpp`) now serialises every entry that touches it: blittable draw/lock/unlock/destroy, `createBlittable`, `EtnaPaintEngine::end`, and `QEtnaviv2dBackingStore::flush`. The mutex is not held while Qt rasterises into a locked image. Test `qt2dbench --threads`: 4 worker threads (QPixmap fill/drawPixmap/drawText/toImage with pixel check) plus the GUI thread, 5 s: bad=0, PASS. No fps change (`--clip` 90 fps, `--matrix` 52/2).
- **KMS scanout format:** dumb-BO `bpp`, DRM fourcc and the imported etnaviv surface format now follow `QT_ETNA2D_FMT` (alpha planes need an alpha format; default stays argb8888 for overlay/composite and xrgb8888 for primary). Build-checked only; the KMS-direct path is still not run on the board (Weston owns the DRM master).
- **Low-bit formats:** `qt2dbench --matrix` takes `QT2D_MATRIX_TOL` (per-channel tolerance, default 8) because 565/4444 quantise against the 8-bit offscreen reference. With `QT_ETNA2D_FMT=rgb565 QT2D_MATRIX_TOL=20` and `argb4444 QT2D_MATRIX_TOL=40`: 52 match / 2 known ellipse-clip DIFF, same as argb8888. ARGB1555 has no Qt image equivalent, so it is covered at the library level: `etna2d_selftest` now checks fill, 1:1 copy and SrcOver on ARGB4444 / ARGB1555 / RGB565 (all pass).
- **wl_output:** the transport binds the first `wl_output`; the default window / ring size is its current mode (rotation swaps), `QT_IVI_GEOMETRY` still overrides, fallback 1024x600. Verified: unset gives 1920x1080 on this setup, `QT_IVI_GEOMETRY=800x480` gives 800x480. Behaviour change: scripts that relied on the 1024x600 default must set `QT_IVI_GEOMETRY`. Resizing the ring after connect (window `resize()`) is not done.

## Step 7: Live output / window size update (replaces the QtWayland migration)
QtWayland replacement dropped: direct KMS must stay supported and shares the backing store, blitter and damage tracking with the Wayland path, and QtWayland cannot drive KMS planes (see discussion, no technical blocker was ever recorded for the hand-written client).

Today the size is read once at connect (`wl_output.mode` after two roundtrips, `QT_IVI_GEOMETRY` override) and the ring never changes (`allocateRing()` once from `integration.cpp`).
- Keep listening to `wl_output.mode` / `done` after connect (current mode only, apply on `done`). Re-evaluate `defaultSize()`; ignore when `QT_IVI_GEOMETRY` is set.
- Transport: `reallocateRing(w, h)`: stop committing, wait for/ignore pending `wl_buffer.release`, destroy the 3 `wl_buffer`s and BOs after `etna2d_finish`, allocate new ones, reset per-slot damage history (`takeDamage` must treat every slot as fully dirty), drop any pending frame callback state.
- Screen: update `mGeometry` / physical size (use `wl_output.geometry` mm) and call `QWindowSystemInterface::handleScreenGeometryChange`; the top-level window follows when it is fullscreen / maximised. `Screen` already has a geometry-update path (`qetnaviv2dscreen.cpp` ~593).
- Backing store: `resize()` reallocates the backing surface for the new size (blitter surfaces are fixed size), force a full flush for the first frame.
- App-driven size: `QWindow::resize()` / `setGeometry()` on the top-level reallocates the ring to the window size (not only the output size); ivi surface size follows. Optional follow-up: `wl_output.scale`.
- Hazards: resize while a frame is in flight; Weston still holding old buffers (do not free before `release` or `finish`); the context mutex from Step 6 must cover the realloc; `QT_ETNA2D_VERIFY_FLUSH` must stay bad=0 across a resize.
- Test:
  - add `qt2dbench --resize`: change the window size repeatedly (`resize()` between frames, e.g. 1024x600 <-> 800x480 <-> 1920x1080) with `VERIFY_FLUSH=1`: bad=0, no crash, no leaked BOs (count stays constant), content correct after each resize.
  - Weston side: change the output mode (`weston.ini` mode or `weston-info` check) and confirm the ring is reallocated and `STAGE` fps is unchanged afterwards.
  - Regression: `--matrix` 52/2, `--clip` fps within noise, `--threads` PASS.
- Pass: no stale/garbled frame after a resize, no leaks, no fps regression.

### Step 7 results (done)
- **Transport:** `resizeRing(size)` (under `etnaCtxMutex`): `etna2d_finish`, destroy the 3 wl_buffers / surfaces / dmabuf fds (destroying a buffer Weston still shows is legal), drop the pending commit, allocate at the new size. `allocateRing`/`freeRing` share one code path. `wl_output` is now listened to after connect: a changed current mode (on `done`) updates the output geometry and calls a hook; ignored when `QT_IVI_GEOMETRY` pins the size. Physical size (mm) from `wl_output.geometry` now feeds the screen DPI.
- **Screen:** `setWaylandGeometry` emits the geometry change and calls `resizeMaximizedWindows()`, so fullscreen/maximised windows follow the output. (Without it Qt does not resize them: found by test.)
- **Backing store:** at present time (`flush`), a top-level `Qt::Window` whose backing store size differs from the ring resizes the ring (stale map cleared, full flush). So `resize()`, `showFullScreen()` and output changes all end in one reallocation; popups/tooltips never touch the ring. `QT_IVI_GEOMETRY` pins the ring (old behaviour) for scripts.
- **Tests:** `qt2dbench --resize` (window cycled through 640x400, 800x480, 1024x600, 320x240, 1280x720; fd count before/after; `VERIFY_FLUSH`): PASS, 22 resizes, fds 15 -> 15, also with `QT_ETNA2D_TEST_DROP=3`. `QT_ETNA2D_TEST_OUTPUT=WxH` (debug) injects a wl_output mode change 1 s after the first frame, since Weston cannot change mode at runtime: output change -> screen geometry -> window 800x480 -> ring 800x480, `VERIFYFLUSH bad=0`. Regression: `--matrix` 52/2, `--threads` PASS, `--clip` 90 fps, plain `--damage` 1080p 88 fps.
- **Behaviour change:** the ring is now the top-level window size, not the output size (with `QT_IVI_GEOMETRY` unset). A non-fullscreen window gets a ring and ivi surface of its own size. A short-lived 640x480 ring appears at start before a widget goes fullscreen (cost: one extra allocation).
- **Not done:** `wl_output.scale`, multiple outputs, a real Weston mode switch (only injected), rotation of an existing window.

## Step 8: Direct KMS alignment (fixed screen, no live modeset)
Scope decisions (user): the screen is fixed; no live modeset or hotplug. Screen size comes from the connector's preferred mode at startup (no hardcoded size; `QT_IVI_GEOMETRY` is a Wayland-only override and is ignored here). Primary plane and overlay plane are both implemented and tested on the shared code, but only the primary plane is exposed/documented. The self-made composite mode (video on primary + UI on overlay) is not exposed and not touched: the only compositor is Weston, so composite code stays as is (no new work, no regression gate). Wayland stays the primary path.

### 8.0 Baseline (before any change)
- Stop Weston on the board (DRM master), run primary plane: `QT_QPA_PLATFORM=etnaviv2d` (plane unset = primary) with `qt2dbench --matrix`, `--clip`, `--damage` + `QT_ETNA2D_VERIFY_FLUSH=1`, `--threads`, `--resize`, and `test_kms_thread`, `test_screen`. Same with `QT_ETNAVIV2D_PLANE=overlay`.
- Save raw output to `qpa/results/step8-kms-baseline-*.txt`. Record what already fails (KMS has not run since Step 3).
- Add counters like the Wayland ones: flips presented, flips deferred, `ETNAFINAL pending_drop=`.
- Pass: baseline recorded, no crash; known failures listed.

### 8.1 Non-blocking flip (the deferred part of Step 4)
- `pageFlip()` commits `NONBLOCK | PAGE_FLIP_EVENT` and returns; no `select()` while holding `m_flipMutex`.
- A `QSocketNotifier` on the card fd (GUI thread) runs `drmHandleEvent`; the flip handler clears `m_flipPending`, swaps front/back, then calls the backing store `presentPending()`.
- `flush()` while a flip is pending: damage already accumulated by `takeDamage`; set `m_dropped` and return (same as Wayland ring-full). One pending frame at most. With the 2-buffer scanout the blit target is the back buffer, which is not on screen, so it is only the flip commit that is deferred; the back buffer must not be blitted again while its flip is pending (that would tear): defer the whole flush, not only the commit.
- Keep: `m_shuttingDown` guard, `EINTR` on teardown (drain/cancel the notifier, wait the last event once in the destructor), render-thread flushes (mutex), `QT_ETNA2D_NO_REPRESENT` A/B.
- Composite-only code (video staging, release callback) is not changed in behaviour; it must still compile and the legacy blocking wait may stay for that mode only.
- Test: `--damage` with `QT_ETNA2D_TEST_DROP` and `VERIFY_FLUSH` (bad=0), idle-tail check `pending_drop=0`, `--clip` paced 60 fps (frame time variance, no GUI block: CPU time in `select` ~ 0), 5 min soak without tearing/stuck frame (visual).
- Pass: no GUI-thread block on vblank, no stuck frame, fps >= baseline.

### 8.2 Fixed screen policy
- Screen geometry = connector mode, read once, no constants. Verify no remaining literal sizes in `qetnaviv2dscreen.cpp` / integration for the KMS path.
- Fullscreen/maximised windows already follow it (`resizeMaximizedWindows`). A smaller top-level keeps its own backing store size and is blitted at (0,0) of the scanout buffer; the rest of the buffer is cleared once at allocation (no stale garbage). A larger window is clipped to the scanout size. Document it; no ring-like resize.
- Test: `--resize` in KMS mode (window size cycles, scanout stays fixed): no crash, no fd/BO leak, `VERIFY_FLUSH` bad=0 (verify compares the visible rect only).

### 8.3 Formats and planes
- Run the 16-bit scanout formats (`QT_ETNA2D_FMT=rgb565`, `argb4444`) on primary and overlay: modeset accepted, `--matrix` with `QT2D_MATRIX_TOL`, visual check.
- Overlay plane: alpha format required (argb); primary default stays xrgb8888. Overlay is exercised by the same tests but over a Weston-less black primary; it stays unexposed.
- Primary-plane underflow poll with the overlay at 32 bpp (the unmeasured Step 3 pass item) is dropped: overlay is not exposed, so only the primary-alone case is measured.

### 8.4 Wrap-up
- Add the KMS gate to `reminder.md` / README of `qpa` (Weston stopped, commands, expected: `--matrix` 52/2, `--threads` PASS, `--damage` bad=0, `pending_drop=0`).
- Update this plan (results table), tracking row, and the Step 4 "KMS deferred" note. Commit per sub-step (8.0 baseline, 8.1, 8.2/8.3 docs+tests).

### Out of scope
Live modeset / hotplug / mode change, multiple connectors, composite mode changes, plane scaling or rotation, `wl_output.scale`.

## Tracking
| Step | Status | fps / metric before | after | commit |
|---|---|---|---|---|
| 0 | done | baseline in results/ | counters added | see git log |
| 1 | done | translucent fill full 35.3 ms | 7.0 ms (GPU-only paths -5..-18%, see Step 2) | see git log |
| 2 | done (submit on painter end) | mixed GPU+CPU loop 35.4 ms | 29.3 ms | see git log |
| 3 | done (Wayland) | 1080p partial update 61 -> 269 loops/s | copy 245 -> 34 MB/s | see git log |
| 4 | done (Wayland repaint-on-release; KMS deferred) | dropped frames lost | all presented, stale UI fixed | see git log |
| 5 | done (policy measured, thresholds 0) | empty finish 118 us | 0 us | see git log |
| 5b | done (opacity + text on the DE) | opacity full 66 ms, text 113 glyphs 2.5 ms | 8.7 ms, 1.3 ms | see git log |
| 6 | done (mutex, KMS fmt, 16-bit tests, wl_output default; no live resize) | threads untested | --threads PASS | see git log |
| 7 | done (live output/window resize; QtWayland migration dropped) | ring fixed at connect | follows window/output, --resize PASS | see git log |
| 8 | todo (direct KMS: baseline, non-blocking flip, fixed screen, formats; primary+overlay, no composite) | KMS blocks a vblank per flush, not run since Step 3 | | |
