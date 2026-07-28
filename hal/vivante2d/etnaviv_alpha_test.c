/*
 * etnaviv_alpha_test.c — GC520 DE alpha-blend (BIT_BLT + PE) validation.
 *
 * Proves the pixel engine performs per-pixel alpha blending using
 * premultiplied-alpha "source over":
 *   out.rgb = src.rgb + dst.rgb * (1 - src.a)
 *   out.a   = src.a  + dst.a   * (1 - src.a)
 *
 * The GC520 PE operates in premultiplied space — src.rgb must already be
 * scaled by src.a before being handed to the hardware.  Qt renders into
 * premultiplied buffers internally, so this matches the real Qt paint path.
 *
 * Method:
 *   1. GPU-clear dest to an opaque background colour.
 *   2. CPU-fill source with a premultiplied semi-transparent colour.
 *      Non-premult (A=0x80, R=0xff, G=0, B=0) →
 *      premult     (A=0x80, R=0x80, G=0, B=0)
 *   3. Blend source over the whole dest.
 *   4. Verify against the analytic premult-SrcOver within +/-2/channel.
 *
 * Usage: etnaviv_alpha_test /dev/dri/renderD128 [out.bmp]
 */
#include <stdlib.h>
#include <string.h>

#include "etnaviv_drmif.h"
#include "etna2d_util.h"
#include "write_bmp.h"

#define W       256
#define H       256
#define STRIDE  (W * 4)
#define SIZE    (STRIDE * H)

/* ARGB — source is premultiplied: R = 0xff * 0x80/0xff = 0x80 */
#define DST_BG      0xff0000ffu   /* opaque blue                    */
#define SRC_NP      0x80ff0000u   /* non-premult: 50% red           */
#define SRC_PREMULT 0x80800000u   /* premult:     A=0x80, R=0x80    */
#define TOL         2

/* Analytic premultiplied SrcOver:
 *   out.rgb = src.rgb + dst.rgb * (1 - src.a) / 255
 *   out.a   = src.a  + dst.a   * (1 - src.a) / 255   */
static uint32_t blend_over_premult(uint32_t src_pm, uint32_t dst)
{
	int sa = (src_pm >> 24) & 0xff;
	uint32_t out = 0;
	/* RGB channels */
	for (int shift = 0; shift < 24; shift += 8) {
		int sc = (src_pm >> shift) & 0xff;   /* already premultiplied */
		int dc = (dst    >> shift) & 0xff;
		int v = sc + (dc * (255 - sa) + 127) / 255;
		if (v > 255) v = 255;
		out |= (uint32_t)v << shift;
	}
	/* Alpha channel */
	{
		int da = (dst >> 24) & 0xff;
		int v = sa + (da * (255 - sa) + 127) / 255;
		if (v > 255) v = 255;
		out |= (uint32_t)v << 24;
	}
	return out;
}

int main(int argc, char *argv[])
{
	struct etna2d_ctx c;
	struct etna_bo *src, *dst;
	struct etna_cmd_stream *stream;
	uint32_t *sp, *dp;
	int ret = 1;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s /dev/dri/<renderD1xx> [out.bmp]\n", argv[0]);
		return 2;
	}
	if (getenv("ETNA2D_VERBOSE"))
		etna2d_verbose = 1;

	LOGI("=== GC520 alpha-blend test (premult SrcOver) ===");
	LOGI("dst bg = 0x%08x  src premult = 0x%08x  (non-premult was 0x%08x, alpha=0x%02x)",
	     DST_BG, SRC_PREMULT, SRC_NP, (SRC_PREMULT >> 24) & 0xff);

	if (etna2d_open(&c, argv[1]))
		return 1;

	src = etna2d_bo_new(&c, SIZE, "src");
	dst = etna2d_bo_new(&c, SIZE, "dst");
	if (!src || !dst)
		goto out;

	/* source: premultiplied pixels (CPU) */
	sp = etna_bo_map(src);
	for (int i = 0; i < W * H; i++)
		sp[i] = SRC_PREMULT;
	__sync_synchronize();

	stream = etna_cmd_stream_new(c.pipe, 0x400, NULL, NULL);
	if (!stream) {
		LOGE("etna_cmd_stream_new: %m");
		goto out;
	}

	LOGI("step 1/2: clear dest to background 0x%08x", DST_BG);
	etna2d_fill(stream, dst, STRIDE, 0, 0, W, H, DST_BG);
	etna_cmd_stream_finish(stream);

	LOGI("step 2/2: blend premult source over dest");
	etna2d_blend_over(stream, src, STRIDE, W, H,
			  dst, STRIDE, 0, 0, W, H);
	etna_cmd_stream_finish(stream);

	dp = etna_bo_map(dst);
	if (argc > 2)
		bmp_dump32((void *)dp, W, H, false, argv[2]);

	uint32_t expect = blend_over_premult(SRC_PREMULT, DST_BG);
	uint32_t got    = dp[(H / 2) * W + (W / 2)];
	LOGI("analytic blend = 0x%08x, hw sample = 0x%08x (tol +/-%d)",
	     expect, got, TOL);

	/* sanity: result must differ from both raw inputs */
	if (got == DST_BG) {
		LOGE("result == dst background — blend did not occur");
		goto done;
	}
	if (got == SRC_PREMULT) {
		LOGE("result == premult source — dest was ignored");
		goto done;
	}

	int bad = 0;
	for (int i = 0; i < W * H; i++) {
		if (etna2d_cmp_tol(dp[i], expect, TOL) != 0) {
			LOGE("pixel %d: expected ~0x%08x got 0x%08x",
			     i, expect, dp[i]);
			if (++bad >= 8)
				break;
		}
	}

	if (bad) {
		LOGE("alpha blend mismatch (%d+ bad pixels)", bad);
	} else {
		LOGOK("alpha blend verified across %dx%d within +/-%d/channel",
		      W, H, TOL);
		ret = 0;
	}

done:
	etna_cmd_stream_del(stream);
out:
	if (dst) etna_bo_del(dst);
	if (src) etna_bo_del(src);
	etna2d_close(&c);
	return ret;
}

