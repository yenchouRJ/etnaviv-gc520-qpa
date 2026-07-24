/*
 * kms_test.c — Phase 3 Milestone A: prove on-panel output via legacy KMS.
 *
 * dumb buffer -> drmModeAddFB2 -> CPU-draw a test pattern -> drmModeSetCrtc.
 * No GPU involved: this isolates the display path from the paint path.
 *
 * Usage: kms_test [/dev/dri/card0] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "kms_display.h"

int main(int argc, char **argv)
{
	const char *card = (argc > 1) ? argv[1] : "/dev/dri/card0";
	int secs = (argc > 2) ? atoi(argv[2]) : 5;
	struct kms_display *d = NULL;
	struct kms_fb fb;
	uint32_t *px;
	int w, h;

	if (kms_open(card, &d)) {
		fprintf(stderr, "kms_open failed\n");
		return 1;
	}
	w = kms_width(d);
	h = kms_height(d);

	if (kms_fb_create(d, &fb)) {
		kms_close(d);
		return 1;
	}
	px = kms_fb_map(d, &fb);
	if (!px) {
		kms_fb_destroy(d, &fb);
		kms_close(d);
		return 1;
	}

	/* Test pattern: colour bars + a white diagonal, so a wrong stride or
	 * format is immediately visible on the panel. stride is in bytes. */
	{
		int sp = fb.stride / 4;
		static const uint32_t bar[8] = {
			0x00000000, 0x00ff0000, 0x0000ff00, 0x000000ff,
			0x00ffff00, 0x0000ffff, 0x00ff00ff, 0x00ffffff,
		};
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				uint32_t c = bar[(x * 8) / w];
				if (x == (y * w) / h)   /* diagonal */
					c = 0x00ffffff;
				px[y * sp + x] = c;
			}
	}

	if (kms_set_crtc(d, &fb)) {
		kms_fb_destroy(d, &fb);
		kms_close(d);
		return 1;
	}

	printf("[ OK ] displaying %dx%d test pattern for %ds\n", w, h, secs);
	sleep(secs);

	kms_fb_destroy(d, &fb);
	kms_close(d);
	printf("[ OK ] kms_test done\n");
	return 0;
}
