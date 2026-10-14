# VC8000NanoD DRM driver + GStreamer plugin — Implementation Plan

Target: Nuvoton MA35D1 (kernel 6.6.93, Buildroot `ma35d1_br24_graphic`), VC8000NanoD
H.264 + JPEG decoder with post-processor (PP), no V4L2.
Deliverables:

1. `vc8k` — kernel DRM **render-node** driver (`/dev/dri/renderD129`-ish): GEM/dma-buf
   memory, register-job submit, IRQ, fences, reset.
2. `libvc8k` — userspace library: DRM wrapper + the vendor codec core + a DWL shim over the
   DRM ioctls (same role `libetna2d` plays over `libdrm_etnaviv`).
3. `gst-vc8k` — GStreamer 1.22 plugin: `vc8kh264dec`, `vc8kjpegdec`, dmabuf NV12 output that
   `waylandsink`/`kmssink` already know how to present (the Weston/plane stack from the
   earlier phases is unchanged).

Background: `../dev.md` (research report), vendor tree `drivers/vpu/VC8000NanoD` (nested git
repo, untracked by this repo — treat as read-only vendor drop; we copy what we need).

---

## 0. What the source review changed versus `dev.md`

Facts verified in `Linux_v2/nuvoton/ma35d1/` that drive the whole plan:

| # | Finding | Consequence |
|---|---|---|
| 1 | The codec core is a **stateful, bitstream-in** decoder. `vc_codec_h264_decode()` takes raw Annex-B bytes and does NAL/SPS/PPS/slice-header parsing, POC, DPB, reordering, error concealment itself. The V4L2 "`H264_SLICE`" format/controls are nominal; the glue never uses slice params. | dev.md's "stateless codec frontend / reuse stateless GStreamer element" does **not** apply. There is no stateless path with this core. Our gst element is a normal `GstVideoDecoder` fed byte-stream AUs (after `h264parse`). |
| 2 | The private core (`blob/vc8000_core.o_shipped`, 215 KB) is kernel-independent: plain AArch64 relocatable object, built `-ffreestanding -mgeneral-regs-only -fno-pic`, and its **only** undefined symbols are 21 DWL functions + `memcpy` (`internal/allowed-imports.txt`): `DWLInit/Release`, `DWLReserveHw/HwPipe`, `DWLReleaseHw`, `DWLWriteReg/ReadReg`, `DWLEnableHW/DisableHW`, `DWLWaitHwReady`, `DWLReadAsicID/Config`, `DWLMalloc/FreeRefFrm`, `DWLMalloc/FreeLinear`, `DWLmalloc/free/calloc/memcpy/memset`, `DWLLog`. | The DWL is the seam Hantro designed for a **userspace codec + tiny kernel driver** (original `hx170dec`+`memalloc` split). It is a natural fit for a DRM render node exactly like etnaviv: kernel = memory + submit + IRQ, userspace = all codec logic. |
| 3 | The core keeps **all** decoder state in per-instance handles and a per-core shadow register file that the *shim* owns (`vc_dwl_shadow_regs`). The global-state problems dev.md lists (`_vc8k_vpu`, `vc_engine_registers`, global wait queues, global DMA pool) are all in the *shim*, not the core. | Moving the core to userspace gives multi-instance for free (one process/instance = own shadow regs). Kernel state becomes a single job queue + one register file. |
| 4 | Only when the **PP is enabled in combined (pipeline) mode** does the decoder write pictures to a caller-chosen address (`ppConfig.output.buffer_bus_addr`). Without PP the glue `memcpy`s from the DPB (CPU pointer) into the V4L2 buffer. | Zero-copy output = always run the PP pipeline (also gives NV12/RGB/scale/rotate). One PP output BO address per output picture. |
| 5 | The adapter (`source/adapter/vc_codec_adapter.c`) exposes **single-buffer** PP only; `PPDecSetMultipleOutput` (multibuffer) exists in `ppapi.c` but is not in the ABI. | v1 = one output address programmed before each `decode()`; ABI extension for multibuffer is a tracked stretch item (§7). |
| 6 | Register-write quirks live in the shim: decoder push masks reg2 bit7, writes regs 2..59 then reg1 (enable/IRQ) last; PP push writes regs 61..100, forces **reg91 = 0** (raster out for DCUltra), reg60 last. IRQ: dec = reg1 bit8 (pending), PP = reg60 bit8; status bits 12 RDY, 13 BUS, 14 BUF_EMPTY, 15 ASO, 16 ERROR, 17 SLICE, 18 TIMEOUT; cleared by writing 0. | These move into the kernel job engine, documented + unit-tested. reg91 forcing must be revisited (it is display-path specific). |
| 7 | The V4L2 shim pokes DCUltra (`0x40260000`+`0x1518`, `0x147C`, `0x1400`, `0x15C0`) from the VPU IRQ handler and PP config. | **Dropped entirely.** PP writes to GEM BOs; scanout goes through the existing `ma35-drm` KMS driver via dmabuf/Weston planes. VPU driver never maps the display block. |
| 8 | The address bus is 32-bit (`dma_set_coherent_mask(32)`), there is **no IOMMU** for the VC8K in `ma35d1.dtsi`; every buffer must be physically contiguous. | GEM objects = CMA (`drm_gem_dma_object`). Foreign dmabuf import only if contiguous. Security model below. |
| 9 | Linux_v2 targets 6.12/6.18 (`ma35d1-vc8000-linux-6.18-port.patch`); our tree is **6.6.93**. | Write against 6.6 DRM APIs, using in-tree `etnaviv` (same kernel, already patched by us) as the structural template. |
| 10 | `CONFIG_VIDEO_NUVOTON_VC8K=y` (built-in, `bool`) is bound to the same DT compatible `nuvoton,ma35d1-vc8k`. | Must be switched off in the kernel config before `vc8k` DRM can bind. Kconfig: `depends on !VIDEO_NUVOTON_VC8K`. |
| 11 | Display driver `ma35_plane.c` advertises NV12 but has no `prepare_fb`/dma_resv wait. | v1 completes decode (CPU wait on fence) before handing a buffer downstream; implicit-sync is a later enhancement (needs `drm_gem_plane_helper_prepare_fb` in the display driver). |

### Architecture decision (recorded so it can be challenged)

**Chosen: "etnaviv model".** Thin kernel DRM render node + userspace codec core.

```
gst-launch: filesrc ! h264parse ! vc8kh264dec ! waylandsink   (dmabuf NV12, zero copy)
                                   │
                              gst-vc8k  (GstVideoDecoder, dmabuf pool)
                                   │  vc8k_dec_* (libvc8k public API)
                              libvc8k
                                   ├─ vendor core (blob or source): H.264 / JPEG / PP
                                   ├─ DWL shim (userspace): shadow regs, alloc, enable/wait -> ioctls
                                   └─ vc8k_drm (libdrm-style ioctl wrapper)
                                   │
              ───────────── /dev/dri/renderD1xx ─────────────
                              vc8k.ko  (DRM render node)
                                   ├─ GEM: CMA-backed, PRIME import/export, mmap (WC)
                                   ├─ job engine: drm_sched, one HW core, dec+PP register jobs
                                   ├─ IRQ / timeout / reset / runtime PM
                                   └─ register-address validation (relocs)
                                   │
                              VC8000NanoD @ 0x40290000, SPI 28
```

Alternative considered and **rejected for now**: keep the blob in-kernel and expose a coarse
`DECODE(bitstream_bo, len) -> pictures` ioctl (what the V4L2 shim does). Rejected because it
parses untrusted bitstreams in ring-0 with a proprietary object, puts DPB/EOS/resolution-change
semantics into DRM uAPI (which dev.md warns against), and makes every codec bug a kernel bug.
It remains the **fallback** if the userspace-linking licence check (Open Question Q1) fails —
the kernel half (GEM + job engine + IRQ) is identical, only the DWL shim moves back into the
kernel. Design the kernel uAPI so this fallback costs nothing.

---

## 1. Repository layout (`vpu/vc8k/`)

```
vpu/vc8k/
├── plan.md                        this file
├── kernel/                        vc8k.ko  (out-of-tree Kbuild + in-tree Kconfig copy)
│   ├── Kbuild  Makefile  Kconfig
│   ├── vc8k_drv.c/.h              platform probe, drm_driver, ioctl table, PM
│   ├── vc8k_gem.c/.h              CMA GEM, PRIME, mmap, cpu_prep/fini
│   ├── vc8k_hw.c/.h               MMIO, register push/pull, IRQ, reset, caps, quirks
│   ├── vc8k_sched.c/.h            drm_sched job engine, timeout/reset, fences
│   ├── vc8k_submit.c              SUBMIT ioctl: copy, validate, relocate
│   ├── vc8k_regs.h                register indices/bits, address-register table
│   └── vc8k_trace.h               tracepoints
├── include/uapi/drm/vc8k_drm.h    the uAPI (installed to libdrm-style include path)
├── libvc8k/
│   ├── vc8k_drm.c/.h              ioctl wrapper (device open, bo, submit, wait)
│   ├── dwl/                       DWL shim: init, regs, mem, enable/wait, caps (port of vc_dwl_*.c)
│   ├── core/                      vendor core link (blob + exported_headers, SHA-pinned)
│   ├── vc8k.h                     public API: session create, decode, get_picture, flush
│   └── Makefile                   libvc8k.so.1
├── gst/                           gst-plugin-vc8k (meson or autotools; Makefile like qpa)
│   ├── gstvc8kdec.c/.h            common GstVideoDecoder base
│   ├── gstvc8kh264dec.c  gstvc8kjpegdec.c
│   ├── gstvc8kallocator.c/.h      GEM -> dmabuf GstAllocator + GstBufferPool
│   └── gstvc8k.c                  plugin_init
├── tests/                         per-phase harnesses, output to build/ (like compositor/)
│   ├── p0_baseline/  p1_probe/  p2_gem/  p3_job/  p4_jpeg/  p5_h264/  p6_gst/  p7_e2e/
│   └── vectors/                   small H.264/JPEG streams + golden md5 (see §6)
└── docs/                          register map notes, DWL trace captures, ADRs
```

Rules (same as earlier phases): tests build into `build/`, never `make clean` without asking,
each phase has a runnable acceptance harness, final README section added at the end.

---

## 2. Kernel driver design (`vc8k.ko`)

### 2.1 Device / probe
- Compatible strings kept: `nuvoton,ma35d1-vc8k`, `ma35d0-`, `ma35h0-`. Resources: MMIO
  (`reg`, 4 KB), IRQ SPI 28, clock `VC8K_GATE` (aclk/hclk from the old driver map to what?
  **verify** in dtsi: only one clock listed, old driver asks for `aclk`,`hclk` names —
  check what the V4L2 driver actually does when they're missing), reset line (V4L2 code has it
  `#if 0`'d out; find the real `MA35D1_RESET_VC8K`), optional `memory-region`.
- Obsolete DT props (`pp_out_enable`, `pp_wait_vsync`, `fb_*`, `use_dev_coherent`,
  `nuvoton,sys`) are ignored. `nuvoton,sys` (SYS regmap) is needed only if the clock/reset
  path requires it — check.
- Acquire everything, reset, read caps, **then** `drm_dev_register()` (fixes dev.md's probe
  ordering complaint). Full unwind on every error path (`devm_*` + `drm_dev_enter/exit`).
- `DRIVER_GEM | DRIVER_RENDER` only. No KMS, no card node.
- `dma_set_mask_and_coherent(32)` kept until Q3 (addressing) is answered.

### 2.2 Memory (GEM)
- `drm_gem_dma_object` based; `dma_alloc_wc` behind it. PRIME export/import through the
  helpers; import path relies on contiguity check (rejects scattered dmabufs).
- Pool: global CMA (48 MB today) is too small for 1080p H.264 DPB + PP ring alongside Weston.
  Sizing: 1080p NV12 ≈ 3.1 MB; DPB up to 16 frames + 1 dir-mv; PP out 3–5 buffers.
  **Recommendation:** make `vc8k_buf` a proper `compatible="shared-dma-pool"` region
  (`no-map` is fine, device-coherent pool, replaces the legacy `use_dev_coherent` path) bound
  to the device via `of_reserved_mem_device_init()`, so GEM allocs come from a dedicated pool
  and don't starve display. Falls back to global CMA when no `memory-region`.
  (Note `som-256m.dts` has the region commented out — 256 MB boards need a decision.)
- uAPI exposes each BO's **device address** (needed by the DWL shim) — safe because no IOMMU
  and addresses are re-validated at submit (§2.4).
- Cache model: WC mmap by default. CPU-read-heavy buffers (bitstream) use `GEM_CPU_PREP/FINI`
  with a cached mapping option, or libvc8k keeps a cached staging copy for the parser and
  sends the WC BO to hardware (see §3.3). Decide with a measurement in P2.

### 2.3 Job engine
- One HW core, one register file → one `drm_sched` (`num_rqs` = 1 entity per open file),
  fences via `drm_sched` + `drm_syncobj` for in/out fences. Template: `etnaviv_sched.c`.
- **Job** = ordered list of register-set pushes:
  `{ PP regs (optional) , DEC regs (optional) }` + expected-completion mask (dec / pp / both)
  + timeout. Standalone PP, standalone decode, and combined pipe are the same struct.
- `run_job`: write regs in the documented order (quirks from finding #6), start.
  IRQ: read status, snapshot regs 0..100 into the job, clear (write 0), signal HW fence.
  `timedout_job`: abort (`DEC_ABORT|IRQ_DISABLE`), **wait until HW reports idle** before
  touching registers (release-note 1.1.0: resetting while HW is still decoding halted the
  kernel), then `reset_control` if available, clear regs, resubmit the queue.
- Reserve/Release: **no kernel state** in v1; exclusivity = one job at a time. (Validated
  against the Phase-0 trace that no codec flow depends on holding the core across jobs; if
  one does — e.g. JPEG slice mode — add `RESERVE`/`RELEASE` ioctls with a lease.)
- WAIT returns the register snapshot + status bits, so `DWLWaitHwReady` / pull are a single
  round trip. Error mapping: ready / bus error / buffer-empty / ASO / error / timeout →
  distinct ioctl status codes (feeds `VC_H264_HW_TIMEOUT`, `HW_BUS_ERROR`, …).
- Runtime PM + clock gating around jobs (V4L2 driver never gated; do it after functionality).
- Close/kill with an in-flight job: `drm_sched_entity_flush` + wait for HW completion or
  timeout-abort, never leave the IRQ live with no owner.

### 2.4 Security / validation (no IOMMU)
The register file contains bus addresses that the HW DMAs to/from with arbitrary length.
- Address registers known from `8170enum.h` (95 `*_BASE` HWIF fields: `RLC_VLC`, `DEC_OUT`,
  `REFER0..15`, `DIR_MV`, `QTABLE`, `PP_IN_*`, `PP_OUT_LU/CH`, `ABLEND1/2`, `*_MSB`, …). Build
  a table `(reg index, dec|pp, kind)` from that + the register-map PDF
  (`Decoder_swreg_map_v6.pdf`).
- **SUBMIT carries relocations** `(reg_idx, bo_handle, offset)` (etnaviv style). Kernel writes
  `bo_addr + offset` itself; any address register in the table that is non-zero and has no
  reloc → `-EINVAL`. Optionally bound `offset` to BO size.
- libvc8k builds relocs automatically at push time by scanning shadow regs against the table
  and looking up the owning BO in its allocation map — the core is untouched.
- Staged: **P3** trusted mode (pass values through, render node group-restricted) to unblock
  bring-up; **P8** enforce relocs. Length can't be checked from registers; document residual
  risk (a hostile client with a valid BO can still over-DMA past its buffer — same class as
  other IOMMU-less SoC decoders).

### 2.5 uAPI sketch (`include/uapi/drm/vc8k_drm.h`, versioned)
```
DRM_VC8K_GET_PARAM   { param, value }    hw_id(reg0), synth regs 50/54/57/99/100, uapi ver,
                                         max dims, dec/pp fuses, core count, clk rate
DRM_VC8K_GEM_NEW     { size, flags -> handle }              flags: CACHED, CONTIG(always)
DRM_VC8K_GEM_INFO    { handle -> dev_addr, mmap_offset, size }
DRM_VC8K_GEM_CPU_PREP/FINI { handle, op(to_cpu|to_dev), offset, len }
DRM_VC8K_SUBMIT      { pp_regs*, dec_regs*, reloc*, nrelocs, in_syncobj, out_syncobj,
                       wait_mask, timeout_ms, flags }  -> fence id
DRM_VC8K_WAIT        { fence/syncobj, timeout -> status, regs_out* }
```
PRIME is standard (`DRM_IOCTL_PRIME_*`). Keep `struct` sizes explicit and append-only.

---

## 3. libvc8k

### 3.1 DWL shim mapping (port of `vc_dwl_*.c` / `vc_os_linux.c`)
| DWL | Userspace implementation |
|---|---|
| `DWLInit/Release` | allocate per-client context; shared DRM fd in session |
| `DWLReadAsicID/Config/CoreCount` | `GET_PARAM` once, cache; port `vc_dwl_caps.c` decoding |
| `DWLReserveHw/HwPipe/ReleaseHw` | no ioctl (per-job exclusivity); Pipe = mark PP part of next job |
| `DWLWriteReg/ReadReg` | per-session shadow array (no globals) |
| `DWLEnableHW/DisableHW` | build SUBMIT (regs + relocs) from shadow, async ioctl, store fence |
| `DWLWaitHwReady` | `WAIT` ioctl → copy returned snapshot back into shadow, map status |
| `DWLMallocLinear/RefFrm`, `Free*` | `GEM_NEW` + `GEM_INFO` + `mmap`; keep `{addr→bo}` map for relocs |
| `DWLmalloc/free/calloc/memcpy/memset/Log` | libc / `VC8K_LOG` env → stderr |

Note `DWLcalloc` in the kernel shim deliberately does **not** zero (legacy quirk). Userspace
must use a real `calloc`; verify the core doesn't depend on garbage.

### 3.2 Linking the core (Open Question Q1 + technical spike S1)
The blob is `-fno-pic`, `-mgeneral-regs-only`, AArch64 only. Options, in order of preference:
1. Link `vc8000_core.o_shipped` into `libvc8k.so` (validate: no text-relocation problems /
   `R_AARCH64_ADR_PREL_PG_HI21` against undefined symbols; if it fails, link into a static
   `libvc8k.a`/`-static` core archive and build the plugin around it).
2. Rebuild the core from `source/` with userspace PIC flags (needs licence permission; also
   makes an x86 build against the Cmodel possible for CI).
3. Fallback: in-kernel core (see §0).
Freestanding flags mean no libc dependency inside the core — keep it that way; the ABI test
`nm -u` must equal `allowed-imports.txt`.

### 3.3 Public API (`vc8k.h`, thin, codec-oriented, no DRM types)
```
vc8k_dev_open()                    caps query
vc8k_h264_create / _destroy
vc8k_h264_push(session, au_ptr, len, pts, out_pic_bo)   -> {NEED_MORE, HDRS_RDY(info), PIC(s), ERROR}
vc8k_h264_get_picture(session, &pic)   display-order, EOS/flush aware
vc8k_jpeg_create/decode
vc8k_bo_alloc / _export_dmabuf / _import_dmabuf / _map
```
Cache detail: the core parses the bitstream via its CPU pointer; a WC mapping makes that very
slow. Give the core a **cached staging copy** (`stream`) and the hardware the WC BO
(`stream_bus_address`) — the ABI takes both.

Per-frame PP output flow (single-buffer PP): before each `decode()` call program
`ppConfig.output.buffer_bus_addr` (+ chroma addr) to the *next free output BO* via
`vc_codec_pp_set_config` (glue already does this at runtime for `pp_changed`). If the call
yields no display picture the BO stays unused. Verify with B-frame streams that the emitted
picture always lands in the BO programmed for that call (spike S2).

---

## 4. GStreamer plugin

- Base class: `GstVideoDecoder`. Sink caps: `video/x-h264, stream-format=byte-stream,
  alignment=au` (and `image/jpeg`). Requires `h264parse`.
- Src caps: `video/x-raw(memory:DMABuf), format=NV12` (P1), then `BGRx/RGB16` for PP RGB out
  (useful for the QPA/Qt path), scaling/rotation via PP later.
- `GstAllocator` (`gstvc8kallocator`): `GEM_NEW` → PRIME fd → `gst_dmabuf_allocator_alloc()`;
  `GstBufferPool` with min/max sized from `H264 info` (`picture_buffer_size`,
  `pp_multibuffer_size`) + downstream `GST_QUERY_ALLOCATION`. Accept downstream pools of
  dmabuf only if contiguous (`GEM import` rejects otherwise → fall back to own pool).
- Semantics: `set_format` (headers → `HDRS_RDY` triggers caps negotiation + PP setup),
  `handle_frame`, `drain` (`next_picture(eos=1)`), `flush` (destroy/recreate session),
  `finish`. Latency query from DPB reorder depth. QoS/skip-non-ref via
  `skip_non_reference` input field.
- Error → `GST_ELEMENT_ERROR` with the mapped `VC_H264_*` code; HW timeout → session reset and
  continue from next IDR.
- Ranks: `GST_RANK_PRIMARY+1` so decodebin prefers it over software.
- Reuse existing project pieces: `gst_dmabuf_video` / `gst_plugin_dmabuf_src` test code and the
  patched `waylandsink` (ivi) unchanged; the plugin is a drop-in replacement for the
  "stand-in for a real hardware VPU" mentioned in the top README.
- Buildroot deps to check/enable (Buildroot gst is 1.22.9): `gst1-plugins-base` (video),
  `gst1-plugins-bad` videoparsers (h264parse/jpegparse) + waylandsink, libdrm.

---

## 5. Phases, work items, acceptance tests

Each phase ends with a runnable harness in `tests/pN_*/` and a short entry in a new
`vpu/vc8k/log.md` (mirroring `hal/libetna2d/build/log.md` style). Do not start phase N+1 until
N's acceptance passes on the board.

### P0 — Baseline characterization (on the existing V4L2 driver; board, no new code)
Purpose: golden outputs + hardware facts + behavioural oracle.
- Collect test vectors (§6): 3 H.264 (Baseline no-B, Main with B-frames + reorder,
  High 1080p; plus one interlaced/field if available) and 3 JPEG (baseline 420, 422, large).
  Generate golden `md5` of NV12 output with the V4L2 driver (`Linux/examples/vc8000-h264`
  or `v4l2h264dec`) and with a software decoder (`avdec_h264`) to check equivalence.
- Add trace prints (temporary, on a scratch copy of the V4L2 driver): sequence and arguments of
  reserve/push/wait/release, IRQ status values, PP combined-mode IRQ behaviour (does PP IRQ
  fire and who clears it?), time per frame, register images for one IDR and one P frame.
  **This trace is the specification of the job engine** — save under `docs/traces/`.
- Answer: clocks (`aclk/hclk`), reset line, `reg2 bit7`, `reg91 forcing`, actual VC8K clock
  MHz, achieved fps at 720p/1080p, behaviour of two open handles, timeout/kill behaviour.
- Exit: `tests/p0_baseline/` script reproduces goldens; `docs/hw-facts.md` filled in.

### P1 — Kernel skeleton + probe (`vc8k.ko` without V4L2)
- Kconfig/DT: disable `CONFIG_VIDEO_NUVOTON_VC8K`; build `vc8k` as module; DT `memory-region`
  as `shared-dma-pool`.
- Probe order, clk/reset/IRQ, caps read, render node registered, `GET_PARAM`.
- Test `tests/p1_probe/vc8k_probe`: opens the node, prints hw_id (expect the VC8000NanoD id
  read by `DWLReadAsicID`), synth/fuse decode via ported `vc_dwl_caps` logic; asserts H.264 +
  JPEG + PP support, max width. Also `insmod`/`rmmod` × 50, unbind/bind, no leaks (kmemleak).
- Exit: module loads on the 6.6.93 board with V4L2 driver off; clean dmesg.

### P2 — GEM + dma-buf
- CMA GEM, `GEM_NEW/INFO`, mmap (WC + cached option), PRIME export/import, cpu_prep/fini.
- Tests: alloc/free loops to pool exhaustion (ENOMEM, no leak); mmap write/readback;
  **export to display**: import BO fd into `/dev/dri/card0` (`ma35-drm`), NV12 fill, atomic
  commit on the NV12 overlay plane (reuse `compositor/` test scaffolding); import an
  etnaviv2d-allocated dmabuf into vc8k (contiguity accept/reject); measure cached vs WC read
  bandwidth (decides §3.3 staging).
- Exit: a vc8k BO scans out on a KMS plane via dmabuf.

### P3 — Register-job submit, IRQ, arbitration (trusted mode)
- `SUBMIT/WAIT`, `drm_sched`, IRQ handler, timeout/abort/reset, snapshot regs, quirks.
- Test `tests/p3_job/`: hand-built register image from a P0 trace (one I-frame or one JPEG,
  DEC-only, no PP) replayed by a small C program → compare output BO to golden. Then PP-only
  job (NV12→RGB/scale of a golden frame). Then combined.
- Robustness tests: two processes submitting concurrently; `kill -9` mid-job; deliberately
  bad stream → ERROR status; forced timeout (garbage regs) recovers and next job succeeds;
  1000-job soak; `dmesg` clean.
- Exit: register replay matches golden bit-exact; kill/timeout tests pass.

### P4 — libvc8k + DWL shim + JPEG (first real end-to-end)
JPEG first: single-shot jobs, no DPB/reorder, so it validates the DWL shim, blob linking
(spike S1), caps, relocs generation and error mapping with the least codec complexity.
- Port `vc_dwl_*` to userspace over the ioctls; link core; `vc8k_jpeg_decode()` API.
- Tests: `tests/p4_jpeg/vc8k_jpeg` decodes vectors (420/422/444/440/400, with/without PP scale
  and NV12/RGB out) → md5 vs golden; NULL/garbage/truncated JPEG must return errors, not hang;
  two parallel processes.
- Exit: bit-exact vs P0 goldens; `nm -u` import check; valgrind/ASan-clean shim.

### P5 — H.264 decode with PP zero-copy output
- H.264 session, header handling (`HDRS_RDY` → PP init), DPB (`DWLMallocRefFrm` → GEM), output
  BO programming per §3.3, display-order output, EOS drain, resolution change (destroy/recreate
  DPB), skip-non-ref.
- Tests `tests/p5_h264/`: every vector, frame md5 vs P0 golden, in decode order **and**
  display order, IDR/CRA seek, mid-stream SPS change, truncated/corrupted NAL (concealment
  reported via `error_macroblocks`), 1000-frame soak, fps measurement vs V4L2 baseline (must be
  ≥ it; target 1080p30 HP if the clock allows — see §7).
- Exit: all vectors bit-exact; no leaks (BO count returns to 0); performance recorded.

### P6 — GStreamer plugin
- Element + allocator/pool + caps + drain/flush + errors.
- Tests `tests/p6_gst/`: `gst-inspect`; `filesrc ! h264parse ! vc8kh264dec ! fakesink` md5 vs
  P5 golden; `... ! kmssink` / `... ! waylandsink` visually (dual-plane with Qt UI client using
  `wayland/tests/verify_planes.sh` — video must land on the NV12 plane with no CPU copy:
  check via `perf`/`top` CPU% and `/sys/kernel/debug/dri/*/state`); `jpegdec` on a `multifilesrc`
  slideshow; seek/EOS/flush/pause-resume; run 30 min loop with `gst-launch` restart × 200
  (leak check via BO count in debugfs); `decodebin` autoplug picks vc8k.
- Exit: 1080p (or the P5 ceiling) H.264 file plays smoothly through Weston, video on hardware
  plane, UI client on its own plane.

### P7 — System integration & packaging
- Buildroot packages `vc8k-drm` (kernel-module infra, `SITE_METHOD=local`), `libvc8k`,
  `gst1-vc8k`; kernel config delta (VC8K V4L2 off, DRM_SCHED, CMA/pool sizing); DT delta;
  Yocto mapping row like the existing README table.
- Qt: optional `QMediaPlayer`/GStreamer path check under the etnaviv2d QPA (no VC8K-specific
  Qt code expected).
- Update top-level `README.md` (layout, components, config reference, harness commands).

### P8 — Hardening
- Enforce relocation validation (§2.4); fuzz SUBMIT with random regs/relocs (syzkaller-lite
  script) — kernel must never oops/hang.
- Runtime PM/clock gating; suspend/resume; sysfs/debugfs (`/sys/kernel/debug/dri/N/`: jobs,
  BOs, last regs, IRQ counters); tracepoints.
- Implicit sync: add `drm_gem_plane_helper_prepare_fb` to the display driver + write fence into
  the output BO `dma_resv` at job completion → allows async decode-ahead.
- Optional: multibuffer PP (ABI extension request to the core owner), stateless-style
  multi-process arbitration priorities.

---

## 6. Test vectors & golden data
- Small, committed (`tests/vectors/`, < a few MB): 720p/1080p H.264 BP, MP(B-frames), HP; 3
  JPEGs; each with `*.md5` (NV12, per-frame) produced in P0.
- Large/real content: kept out of git (`.gitignore`d), documented paths (USB `/mnt/vivante2d/`).
- Comparison is bit-exact vs the V4L2 driver (same core → must match). SW decoder (`avdec_h264`)
  comparison is PSNR/visual only (concealment and rounding may differ).
- Host-side: consider `ffmpeg`/`x264` scripts to *generate* vectors reproducibly
  (`tests/vectors/gen.sh`) instead of committing binaries.

---

## 7. Risks, spikes, open questions

### Spikes (do early, each ≤ 2 days, before committing to P4/P5)
- **S1 Blob-in-userspace:** link `vc8000_core.o_shipped` into an aarch64 `.so` and a static
  binary; run with a fake DWL (in-memory regs) to prove it loads and returns `HDRS_RDY` for a
  stream. Decides §0 architecture fallback.
- **S2 Combined-mode PP output addressing with reordering:** with the P0 register traces,
  confirm per-decode-call PP output address semantics for B-frame streams (single-buffer
  mode; `h264PreparePpRun` sometimes runs PP standalone after decode when decode-order ≠
  output-order — two HW jobs per call).
- **S3 IRQ behaviour in pipe mode:** how many IRQs, which status bits, ordering, who clears.

### Open questions (need owner input)
- **Q1 Licence/distribution:** the core and `source/` are Hantro/Nuvoton proprietary. Is
  linking the shipped object into a *userspace* library (and into a GStreamer plugin) allowed
  and distributable? If not → in-kernel fallback (module taint/licence rules apply as today).
- **Q2** Do we need the JPEG *encoder*/H.264 encoder? Current HW/ABI = decode only (variant has
  `num_enc_fmts = 0`). Plan assumes decode only.
- **Q3 32-bit DMA mask:** hardware limit or historical? `DWLReadAsicConfig` exposes `addr64Support`;
  DRAM base is 0x80000000 so ≤ 2 GB boards are fine either way. Confirm for 4 GB SKUs.
- **Q4** Reserved `vc8k_buf` (32 MiB, absent on 256 MB SOM): dedicated `shared-dma-pool` vs
  bigger global CMA. Depends on target board SKUs.
- **Q5** Performance target: 1080p30? 720p60? (feature doc: min clock for 1080p60 HP ≈ 208 MHz
  at 426 cycles/MB; PP RGB pipeline 498 cycles/MB.) Need actual `VC8K_GATE` rate and bus
  bandwidth on the board.
- **Q6** Resolution limit: shim's format table caps at 1920×1920 in places, HW doc says H.264
  ≤ 1920×1080, JPEG ≤ 16368×16368 — reconcile with fuse/synth read at P1.
- **Q7** Do MA35D0/MA35H0 (same compatible family) need to be supported now? Plan targets D1
  only, other variants via DT compatible/`GET_PARAM`.

### Technical risks
| Risk | Mitigation |
|---|---|
| Blob not linkable/usable in userspace (`-fno-pic`, freestanding assumptions) | S1 before any other userspace work; fallback = kernel core |
| CMA fragmentation / exhaustion with DPB + PP + Weston + Qt | dedicated pool (Q4), report `ENOMEM` cleanly, plugin falls back to smaller DPB |
| Cache incoherence (non-coherent DMA, WC vs cached mappings) | explicit cpu_prep/fini; start with WC; test with P2 bandwidth + P5 md5 loops |
| HW hang leaves IRQ storm / kernel halt on close (release note 1.1.0) | abort-then-wait-idle protocol in P3; kill -9 tests |
| Untrusted register programs DMA anywhere | relocs + address-reg table (P8), render node permissions (`video` group) meanwhile |
| Reserve/Release semantics needed across several HW runs (JPEG slice mode, H.264 ASO/multi-run) | P0 traces; add lease ioctls if required |
| PP combined-mode output order vs gst buffer flow | S2; multibuffer ABI extension as stretch |
| Display path can't wait on fences | CPU-wait on completion in v1; add prepare_fb later |
| Unknown quirks (`reg2 bit7`, `reg91 = 0`) | document from P0; keep them in one `vc8k_hw.c` table |

---

## 8. Build & environment notes
- Cross env: `source /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_graphic/output/host/environment-setup`
  (never run `make` on Buildroot itself). Kernel tree:
  `.../ma35d1_br24_graphic/output/build/linux-custom` (6.6.93; `DRM_SCHED=m`,
  `DRM_GEM_DMA_HELPER=y`, `CMA_SIZE_MBYTES=48`, `DRM_ETNAVIV=m` present).
- Out-of-tree module build: `make -C <linux-custom> M=$PWD/kernel modules`. Kernel currently has
  `VIDEO_NUVOTON_VC8K=y` — needs config change + kernel rebuild (owner: user, like the earlier
  etnaviv/DT changes).
- Structural references in-tree (6.6): `drivers/gpu/drm/etnaviv/{etnaviv_drv,gem_prime,sched,gpu}.c`
  (submit/relocs/sched/reset), `drivers/gpu/drm/nuvoton/ma35_plane.c` (importer of our dmabufs).
- Board: `ssh root@192.168.0.102`, tests/artifacts on `/mnt/vivante2d/`.
- Kernel/DT changes are prepared as patches in `vpu/vc8k/patches/` (Kconfig, DT `vc8k_buf`
  pool, defconfig fragment) for the user to apply, same convention as the earlier phases.

## 9. Immediate next actions
1. P0: build a scratch copy of the V4L2 driver with tracing, capture goldens/traces on the board.
2. S1: prove the blob links and runs in userspace with a fake DWL (no hardware needed).
3. Resolve Q1 (licence) and Q4/Q5 (memory/perf targets) — they gate P2/P4 scope.
4. Scaffold `vpu/vc8k/` tree + `kernel/Kbuild` + `include/uapi/drm/vc8k_drm.h` v0.

---

## 10. Implementation status (kernel half + tests)

Implemented in this tree (P1-P3 scope; cross-compiles clean against 6.6.93, **not yet run on
hardware** — the V4L2 driver must be disabled first, see `patches/README.md`):

| Path | Content |
|---|---|
| `include/uapi/drm/vc8k_drm.h` | uAPI v1: GET_PARAM, GEM_NEW, GEM_INFO, SUBMIT (regs+bos+relocs, in/out syncobj), WAIT (status + register snapshot); PRIME via core ioctls |
| `kernel/vc8k_drv.c` | platform probe (clk, optional reset, memory-region, 32-bit mask, caps read before `drm_dev_register`), render-only DRM driver, per-file `drm_sched` entity |
| `kernel/vc8k_gem.c` | CMA/dma-pool GEM (`drm_gem_dma`), mmap offset, PRIME import rejects non-contiguous |
| `kernel/vc8k_hw.c` | legacy register ordering (dec: regs 2..59 then reg1; PP: 61..100 then reg60; reg91=0 via `pp_force_raster`), IRQ + snapshot + ack, abort-wait-idle reset |
| `kernel/vc8k_sched.c` | one HW job at a time, hw fence, watchdog (`timeout_ms`), recover without resubmit |
| `kernel/vc8k_submit.c` | reloc application, address-register table, `reloc_strict=1` rejects raw addresses, implicit fencing through `dma_resv` |
| `tests/` | `t_probe`, `t_gem [--exhaust]`, `t_job` (validation, watchdog recovery, syncobj, 4-proc concurrency, kill -9), `t_replay` (replay a captured register image), `run_tests.sh` |

Assumptions to confirm on hardware (spike S3): combined DEC|PP job completes on the decoder IRQ
only; PP-only completes on the PP IRQ. Reset line/`nuvoton,sys` not used (clock only).
Not yet done: runtime PM, `drm_dev_enter` guards on ioctls, debugfs, libvc8k/gst (P4+).
