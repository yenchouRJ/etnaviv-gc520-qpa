/*
 * etna2d_selftest.c — libetna2d Phase 2 acceptance test.
 *
 * Exercises the whole public API surface — device open, surface create,
 * batched context, all four ops, flush/finish — and CPU-verifies the
 * resulting pixels. Unlike the Phase 1 tests (one op per submit, raw
 * register sequences), this proves the *library* abstraction and its
 * batching path: several ops accumulate into one command stream and are
 * submitted together.
 *
 * Usage: etna2d_selftest /dev/dri/renderD128
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etna2d.h"

#define W 256
#define H 256

static int fails;

static void logger(int level, const char *msg, void *user)
{
	static const char *tag[] = { "FAIL", "WARN", "INFO", "DBG " };
	(void)user;
	fprintf(stderr, "[%s] %s\n", tag[level < 0 ? 0 : (level > 3 ? 3 : level)], msg);
}

static void ok(const char *what, int cond)
{
	if (cond) {
		printf("[ OK ] %s\n", what);
	} else {
		printf("[FAIL] %s\n", what);
		fails++;
	}
}

static int tol(uint32_t g, uint32_t e, int t)
{
	for (int sh = 0; sh < 32; sh += 8) {
		int d = ((int)((g >> sh) & 0xff)) - ((int)((e >> sh) & 0xff));
		if (d < 0) d = -d;
		if (d > t) return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	struct etna2d_device  *dev = NULL;
	struct etna2d_context *ctx = NULL;
	struct etna2d_surface *src = NULL, *dst = NULL, *big = NULL;
	uint32_t *sp, *dp, *bp;
	int rc, verbose = !!getenv("ETNA2D_VERBOSE");

	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/dri/renderD128\n", argv[0]);
		return 2;
	}
	if (verbose)
		etna2d_set_logger(logger, NULL);

	rc = etna2d_device_open(argv[1], &dev);
	ok("device_open", rc == ETNA2D_OK);
	if (rc != ETNA2D_OK)
		return 1;
	printf("       GC%03x rev 0x%04x FEATURES_0=0x%08llx\n",
	       etna2d_device_model(dev), etna2d_device_revision(dev),
	       (unsigned long long)etna2d_device_features0(dev));

	ok("surface_create src", etna2d_surface_create(dev, W, H, &src) == ETNA2D_OK);
	ok("surface_create dst", etna2d_surface_create(dev, W, H, &dst) == ETNA2D_OK);
	ok("surface_create big", etna2d_surface_create(dev, W, H, &big) == ETNA2D_OK);
	ok("context_create",     etna2d_context_create(dev, 8, &ctx) == ETNA2D_OK);
	if (!src || !dst || !big || !ctx)
		goto out;

	/* premultiplied semi-transparent red source (A=0x80, R=0x80) */
	sp = etna2d_surface_map(src);
	for (int i = 0; i < W * H; i++)
		sp[i] = 0x80800000u;
	etna2d_surface_cpu_fini(src);

	/* --- Batch 1: fill + opaque blit + blend, all in ONE submit ------- */
	etna2d_fill(ctx, dst, 0, 0, W, H, 0xff0000ffu);          /* blue bg    */
	etna2d_fill(ctx, dst, 32, 32, 64, 64, 0xff00ff00u);      /* green box  */
	etna2d_blend_over(ctx, src, dst, 0, 0, W, H);            /* red 50% over */
	rc = etna2d_finish(ctx);
	ok("batched finish (3 ops, 1 submit)", rc == ETNA2D_OK);

	dp = etna2d_surface_map(dst);
	/* blend over blue bg: out = src.rgb + dst.rgb*(1-0x80/255)
	 *   R=0x80, G=0x00, B~0x7f, A~0xff -> 0xff80007f */
	ok("blend over bg pixel", tol(dp[(H/2)*W + W/2], 0xff80007fu, 2));
	/* blend over the green box region (px 48,48): dst was 0xff00ff00
	 *   R=0x80, G=0x00+0xff*127/255=0x7f, B=0x00, A=0xff -> 0xff807f00 */
	ok("blend over box pixel", tol(dp[48*W + 48], 0xff807f00u, 2));

	/* --- Batch 2: opaque blit sub-rect, separate submit --------------- */
	etna2d_fill(ctx, big, 0, 0, W, H, 0xff123456u);
	etna2d_blit(ctx, src, 0, 0, big, 64, 64, 128, 96,
		    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
	rc = etna2d_finish(ctx);
	ok("blit finish", rc == ETNA2D_OK);

	bp = etna2d_surface_map(big);
	ok("blit copied region", bp[(64+10)*W + (64+10)] == 0x80800000u);
	ok("blit background intact", bp[10*W + 10] == 0xff123456u);
	ok("blit outside region", bp[(64+96+5)*W + (64+128+5)] == 0xff123456u);

	/* --- Batch 3: 2x stretch --------------------------------------- */
	{
		struct etna2d_surface *small = NULL;
		uint32_t *smp;
		etna2d_surface_create(dev, 128, 128, &small);
		smp = etna2d_surface_map(small);
		for (int y = 0; y < 128; y++)
			for (int x = 0; x < 128; x++) {
				int q = (x >= 64) | ((y >= 64) << 1);
				static const uint32_t col[4] = {
					0xffff0000u, 0xff00ff00u,
					0xff0000ffu, 0xffffffffu };
				smp[y*128 + x] = col[q];
			}
		etna2d_surface_cpu_fini(small);

		etna2d_fill(ctx, dst, 0, 0, W, H, 0xff000000u);
		etna2d_stretch(ctx, small, dst, 0, 0, W, H);
		etna2d_finish(ctx);

		dp = etna2d_surface_map(dst);
		ok("stretch TL red",   dp[64*W + 64]   == 0xffff0000u);
		ok("stretch TR green", dp[64*W + 192]  == 0xff00ff00u);
		ok("stretch BL blue",  dp[192*W + 64]  == 0xff0000ffu);
		ok("stretch BR white", dp[192*W + 192] == 0xffffffffu);
		etna2d_surface_destroy(small);
	}

out:
	etna2d_context_destroy(ctx);
	etna2d_surface_destroy(big);
	etna2d_surface_destroy(dst);
	etna2d_surface_destroy(src);
	etna2d_device_close(dev);

	printf(fails ? "\n=== %d CHECK(S) FAILED ===\n" : "\n=== ALL CHECKS PASSED ===\n", fails);
	return fails ? 1 : 0;
}
