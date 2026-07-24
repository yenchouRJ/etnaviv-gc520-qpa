# GC520 2D Acceleration — Development Log

---

## Phase 1 — DE Operation Validation

**Hardware:** Nuvoton MA35D1, Vivante GC520 rev 0x5341  
**GPU FEATURES_0:** `0xe02c7eca`  
**Test node:** `/dev/dri/renderD128`  
**Source:** `tests/vivante2d/`

### Tests

All three tests pass. Source: `tests/vivante2d/`.

#### BIT_BLT (`etnaviv_bitblt_test`)

Copies a 128×96 sub-window from a position-dependent ARGB gradient source
(origin `32,48`) into an offset in the destination (origin `16,8`).
Destination was pre-cleared to sentinel `0xff102030` by a GPU CLEAR op.
Every pixel verified: blitted region matches source, background intact.

```
[ OK ] BIT_BLT verified: 128x96 window copied, background intact
```

#### Alpha blend (`etnaviv_alpha_test`)

**GC520 PE operates in premultiplied-alpha space.**
Source must be premultiplied before submission (`0x80ff0000` non-premult
→ `0x80800000` premult). Blend factors: src = `DE_BLENDMODE_ONE`,
dst = `DE_BLENDMODE_INVERSED`, implementing premultiplied SrcOver:

```
out.rgb = src.rgb + dst.rgb × (1 − src.a) / 255
out.a   = src.a  + dst.a   × (1 − src.a) / 255
```

Analytic result `0xff80007f`, hardware produced `0xff80007e` (1 LSB
rounding in B channel). Verified across all 65536 pixels within ±2/channel.

```
[INFO] analytic blend = 0xff80007f, hw sample = 0xff80007e (tol +/-2)
[ OK ] alpha blend verified across 256x256 within +/-2/channel
```

**Phase 4 note:** Qt's `QRasterPaintEngine` produces
`QImage::Format_ARGB32_Premultiplied` internally, matching the hardware
expectation exactly — no pixel conversion needed in the QPA plugin.

#### STRETCH_BLT (`etnaviv_stretch_test`)

2× upscale of a 128×128 4-quadrant source (R/G/B/W) to 256×256.
Stretch factors: `x=0x07f7f y=0x07f7f` (`.16` fixed-point,
formula `(src−1 << 16) / (dst−1)`). Interior samples at 1/4 and 3/4 points
of each destination quadrant all correct.

```
[ OK ] STRETCH_BLT verified: 4 quadrants scaled and placed 2x
```

### What is now proven

| DE operation | Register path | Status |
|---|---|---|
| Solid fill | `DEST_CONFIG_COMMAND_CLEAR` + `CLEAR_PIXEL_VALUE32` | ✅ (from Phase 0) |
| Opaque copy | `DEST_CONFIG_COMMAND_BIT_BLT`, `SRC_ADDRESS/STRIDE/CONFIG`, ROP4 `0xCC` | ✅ |
| Alpha blend | BIT_BLT + `ALPHA_CONTROL_ENABLE_ON`, `ALPHA_MODES` (premult SrcOver) | ✅ |
| Scale | `DEST_CONFIG_COMMAND_STRETCH_BLT`, `STRETCH_FACTOR_LOW/HIGH` | ✅ |

Command stream, FE, GPU MMU (IOMMU v2), fence, and all four DE commands
are confirmed working end-to-end on GC520 rev 0x5341.

### Shared helper layer (`etna2d_util`)

`etna2d_util.c/.h` packages the four validated op-builders as the seed of
the Phase 2 `libetna2d` module. Each function emits a complete, stateless
DE command sequence (full state setup + `DRAW_2D` rectangle) into an
`etna_cmd_stream`. No state leaks between ops.
