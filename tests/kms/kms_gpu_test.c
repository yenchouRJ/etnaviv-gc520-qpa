/*
 * kms_gpu_test.c — Phase 3 Milestone B: GPU renders directly into scanout.
 *
 * Proves the zero-copy pipeline end to end:
 *   card0 dumb buffer -> PRIME export -> etna2d_surface_from_dmabuf
 *   -> GC520 DE fills/blits INTO the scanout BO -> drmModeSetCrtc -> panel.
 *
 * No CPU pixel writes to the scanout: every pixel on screen is produced by
 * the 2D engine writing through etnaviv's MMU into the display's contiguous
 * buffer.
 *
 * Usage: kms_gpu_test [/dev/dri/card0] [/dev/dri/renderD128] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kms_display.h"
#include "etna2d.h"

static void logger(int lvl, const char *msg, void *u)
{
	(void)u;
	if (lvl <= 2) fprintf(stderr, "[etna2d] %s\n", msg);
}

int main(int argc, char **argv)
{
	const char *card   = (argc > 1) ? argv[1] : "/dev/dri/card0";
	const char *render = (argc > 2) ? argv[2] : "/dev/dri/renderD128";
	int secs = (argc > 3) ? atoi(argv[3]) : 5;

	struct kms_display   *disp = NULL;
	struct etna2d_device *dev  = NULL;
	struct etna2d_context *ctx = NULL;
	struct etna2d_surface *scanout = NULL, *sprite = NULL;
	struct kms_fb fb;
	int dmabuf = -1, w, h, rc = 1;

	etna2d_set_logger(logger, NULL);

	if (kms_open(card, &disp))
		return 1;
	w = kms_width(disp);
	h = kms_height(disp);

	if (etna2d_device_open(render, &dev)) {
		fprintf(stderr, "etna2d_device_open failed\n");
		goto out;
	}
	if (etna2d_context_create(dev, 16, &ctx))
		goto out;

	/* 1. scanout dumb buffer on the display side */
	if (kms_fb_create(disp, &fb))
		goto out;

	/* 2. export -> 3. import into etnaviv */
	dmabuf = kms_fb_export_dmabuf(disp, &fb);
	if (dmabuf < 0)
		goto out;
	if (etna2d_surface_from_dmabuf(dev, dmabuf, w, h, fb.stride, &scanout)) {
		fprintf(stderr, "import scanout into etnaviv failed\n");
		goto out;
	}
	printf("[ OK ] scanout %dx%d imported into etnaviv (stride=%u)\n",
	       w, h, fb.stride);

	/* a small premultiplied translucent sprite to blend on top */
	if (etna2d_surface_create(dev, 128, 128, &sprite))
		goto out;
	{
		uint32_t *sp = etna2d_surface_map(sprite);
		for (int i = 0; i < 128 * 128; i++)
			sp[i] = 0x80800000u;   /* premult 50% red */
		etna2d_surface_cpu_fini(sprite);
	}

	/* 4. GPU renders INTO the scanout BO: bg fill + colour boxes + blend */
	etna2d_fill(ctx, scanout, 0, 0, w, h, 0xff202020u);
	etna2d_fill(ctx, scanout, 0,      0,      w / 2, h / 2, 0xffff0000u);
	etna2d_fill(ctx, scanout, w / 2,  0,      w / 2, h / 2, 0xff00ff00u);
	etna2d_fill(ctx, scanout, 0,      h / 2,  w / 2, h / 2, 0xff0000ffu);
	etna2d_fill(ctx, scanout, w / 2,  h / 2,  w / 2, h / 2, 0xffffffffu);
	etna2d_blend_over(ctx, sprite, scanout,
			  (w - 128) / 2, (h - 128) / 2, 128, 128);
	if (etna2d_finish(ctx)) {          /* blocks on the GPU fence */
		fprintf(stderr, "etna2d_finish failed\n");
		goto out;
	}

	/* 5. present */
	if (kms_set_crtc(disp, &fb))
		goto out;

	printf("[ OK ] GPU-rendered scanout on panel for %ds\n", secs);
	sleep(secs);
	rc = 0;

out:
	if (dmabuf >= 0) close(dmabuf);
	etna2d_surface_destroy(sprite);
	etna2d_surface_destroy(scanout);
	kms_fb_destroy(disp, &fb);
	etna2d_context_destroy(ctx);
	etna2d_device_close(dev);
	kms_close(disp);
	printf(rc ? "[FAIL] kms_gpu_test\n" : "[ OK ] kms_gpu_test done\n");
	return rc;
}
