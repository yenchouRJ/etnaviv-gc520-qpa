/*
 * etnaviv_bitblt_test.c — GC520 DE BIT_BLT validation.
 *
 * Proves the drawing engine can fetch a source surface, apply ROP4 SRCCOPY,
 * and write the destination — i.e. a real hardware 2D blit, beyond the
 * CLEAR-only path exercised by etnaviv_2d_test.c.
 *
 * Method:
 *   1. Fill a source BO with a deterministic non-trivial gradient (CPU).
 *   2. Clear a destination BO to a sentinel via the GPU (etna2d_fill).
 *   3. BIT_BLT a sub-rectangle of the source to an offset in the dest.
 *   4. Read back and verify every dest pixel: blitted region must equal the
 *      corresponding source pixels; everything else must stay the sentinel.
 *
 * Usage: etnaviv_bitblt_test /dev/dri/renderD128 [out.bmp]
 */
#include <stdlib.h>
#include <string.h>

#include "etnaviv_drmif.h"
#include "etna2d_util.h"
#include "write_bmp.h"

#define W        256
#define H        256
#define STRIDE   (W * 4)
#define SIZE     (STRIDE * H)

#define SENTINEL 0xff102030u   /* dest background, must survive outside blit */

/* Blit a BLW x BLH window from source (SX,SY) to dest (DX,DY). */
#define SX  32
#define SY  48
#define DX  16
#define DY  8
#define BLW 128
#define BLH 96

static inline uint32_t src_pixel(int x, int y)
{
	/* deterministic, position-dependent, all channels non-zero-ish */
	return 0xff000000u
	     | ((uint32_t)(x & 0xff) << 16)
	     | ((uint32_t)(y & 0xff) << 8)
	     | (uint32_t)((x ^ y) & 0xff);
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

	LOGI("=== GC520 BIT_BLT test ===");

	if (etna2d_open(&c, argv[1]))
		return 1;

	src = etna2d_bo_new(&c, SIZE, "src");
	dst = etna2d_bo_new(&c, SIZE, "dst");
	if (!src || !dst)
		goto out;

	/* 1. CPU-fill source with the gradient. */
	sp = etna_bo_map(src);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			sp[y * W + x] = src_pixel(x, y);
	__sync_synchronize();   /* flush write-combine buffer before GPU reads */
	LOGD("source filled: [0]=0x%08x [center]=0x%08x",
	     sp[0], sp[(H/2) * W + (W/2)]);

	stream = etna_cmd_stream_new(c.pipe, 0x400, NULL, NULL);
	if (!stream) {
		LOGE("etna_cmd_stream_new: %m");
		goto out;
	}

	/* 2. GPU clear dest to sentinel. */
	LOGI("step 1/2: clear dest to sentinel 0x%08x", SENTINEL);
	etna2d_fill(stream, dst, STRIDE, 0, 0, W, H, SENTINEL);
	etna_cmd_stream_finish(stream);   /* submit + wait fence */

	/* 3. GPU blit sub-rectangle. */
	LOGI("step 2/2: blit src(%d,%d %dx%d) -> dst(%d,%d)",
	     SX, SY, BLW, BLH, DX, DY);
	etna2d_blit(stream, src, STRIDE, W, H, SX, SY,
		    dst, STRIDE, DX, DY, BLW, BLH);
	etna_cmd_stream_finish(stream);

	/* 4. Verify. */
	dp = etna_bo_map(dst);
	if (argc > 2)
		bmp_dump32((void *)dp, W, H, false, argv[2]);

	int bad = 0;
	for (int y = 0; y < H && bad < 8; y++) {
		for (int x = 0; x < W; x++) {
			int in_blit = (x >= DX && x < DX + BLW &&
				       y >= DY && y < DY + BLH);
			uint32_t expect = in_blit
				? src_pixel(SX + (x - DX), SY + (y - DY))
				: SENTINEL;
			if (dp[y * W + x] != expect) {
				LOGE("(%d,%d) %s: expected 0x%08x got 0x%08x",
				     x, y, in_blit ? "blit" : "bg",
				     expect, dp[y * W + x]);
				if (++bad >= 8)
					break;
			}
		}
	}

	if (bad) {
		LOGE("BIT_BLT mismatch (%d+ bad pixels)", bad);
	} else {
		LOGOK("BIT_BLT verified: %dx%d window copied, background intact",
		      BLW, BLH);
		ret = 0;
	}

	etna_cmd_stream_del(stream);
out:
	if (dst) etna_bo_del(dst);
	if (src) etna_bo_del(src);
	etna2d_close(&c);
	return ret;
}
