/*
 * etnaviv_stretch_test.c — GC520 DE STRETCH_BLT validation.
 *
 * Proves the drawing engine scales a source surface to a differently-sized
 * destination (point-sampled stretch), correctly placing the scaled result.
 *
 * Method:
 *   1. CPU-fill a small source with four solid quadrants (R/G/B/W).
 *   2. GPU-clear dest to a sentinel.
 *   3. STRETCH_BLT source (SRCxSRC) -> dest (DSTxDST), a 2x upscale.
 *   4. Sample well inside each expected destination quadrant and confirm
 *      the colour (interior sampling avoids edge/rounding ambiguity).
 *
 * Usage: etnaviv_stretch_test /dev/dri/renderD128 [out.bmp]
 */
#include <stdlib.h>
#include <string.h>

#include "etnaviv_drmif.h"
#include "etna2d_util.h"
#include "write_bmp.h"

#define SRC     128
#define DST     256
#define SRC_STR (SRC * 4)
#define DST_STR (DST * 4)
#define SRC_SZ  (SRC_STR * SRC)
#define DST_SZ  (DST_STR * DST)

#define C_RED   0xffff0000u   /* top-left     */
#define C_GREEN 0xff00ff00u   /* top-right    */
#define C_BLUE  0xff0000ffu   /* bottom-left  */
#define C_WHITE 0xffffffffu   /* bottom-right */
#define SENT    0xff123456u

static uint32_t quad_color(int x, int y, int half)
{
	if (y < half) return (x < half) ? C_RED  : C_GREEN;
	else          return (x < half) ? C_BLUE : C_WHITE;
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

	LOGI("=== GC520 STRETCH_BLT test (%dx%d -> %dx%d, 2x) ===",
	     SRC, SRC, DST, DST);

	if (etna2d_open(&c, argv[1]))
		return 1;

	src = etna2d_bo_new(&c, SRC_SZ, "src");
	dst = etna2d_bo_new(&c, DST_SZ, "dst");
	if (!src || !dst)
		goto out;

	/* quadrant source (CPU) */
	sp = etna_bo_map(src);
	for (int y = 0; y < SRC; y++)
		for (int x = 0; x < SRC; x++)
			sp[y * SRC + x] = quad_color(x, y, SRC / 2);
	__sync_synchronize();

	stream = etna_cmd_stream_new(c.pipe, 0x400, NULL, NULL);
	if (!stream) {
		LOGE("etna_cmd_stream_new: %m");
		goto out;
	}

	LOGI("step 1/2: clear dest to sentinel 0x%08x", SENT);
	etna2d_fill(stream, dst, DST_STR, 0, 0, DST, DST, SENT);
	etna_cmd_stream_finish(stream);

	LOGI("step 2/2: stretch-blit source -> dest");
	etna2d_stretch(stream, src, SRC_STR, SRC, SRC, dst, DST_STR, DST, DST);
	etna_cmd_stream_finish(stream);

	dp = etna_bo_map(dst);
	if (argc > 2)
		bmp_dump32((void *)dp, DST, DST, false, argv[2]);

	/* Sample interior of each destination quadrant (1/4 and 3/4 points). */
	int q = DST / 4;      /* 64  */
	int bad = 0;
	bad += etna2d_check_px(dp, DST, q,         q,         C_RED,   "stretch TL");
	bad += etna2d_check_px(dp, DST, DST - q,   q,         C_GREEN, "stretch TR");
	bad += etna2d_check_px(dp, DST, q,         DST - q,   C_BLUE,  "stretch BL");
	bad += etna2d_check_px(dp, DST, DST - q,   DST - q,   C_WHITE, "stretch BR");
	/* extra interior points closer to centre */
	bad += etna2d_check_px(dp, DST, DST/2 - 8, DST/2 - 8, C_RED,   "stretch TL2");
	bad += etna2d_check_px(dp, DST, DST/2 + 8, DST/2 + 8, C_WHITE, "stretch BR2");

	if (bad) {
		LOGE("STRETCH_BLT mismatch (%d bad sample points)", -bad);
	} else {
		LOGOK("STRETCH_BLT verified: 4 quadrants scaled and placed 2x");
		ret = 0;
	}

	etna_cmd_stream_del(stream);
out:
	if (dst) etna_bo_del(dst);
	if (src) etna_bo_del(src);
	etna2d_close(&c);
	return ret;
}
