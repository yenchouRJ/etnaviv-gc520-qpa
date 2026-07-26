/*
 * bench2d.c — Phase 3 Milestone C+D: double-buffered scanout benchmark.
 *
 * The headline accel-vs-CPU number, with no toolkit in the path. Renders an
 * animated scene of N translucent sprites over a background, double-buffered
 * on two PRIME-shared scanout buffers, and reports FPS.
 *
 * Two modes draw the SAME scene so the only variable is who touches pixels:
 *   --gpu   GC520 DE via libetna2d (fill + premult SrcOver blend), batched
 *   --cpu   software: memset bg + per-pixel premultiplied source-over
 * Both composite the identical premultiplied 50%-red sprite, so the result
 * is pixel-equivalent and only the compositing engine differs.
 *
 * Presentation:
 *   default        Double buffering (Milestone C): render the back buffer,
 *                  page-flip, wait the vblank event, swap. Reports two rates:
 *                    render rate  — GPU-fenced fill-rate (or CPU loop time)
 *                    present rate — end-to-end, vsync-capped
 *   --no-vsync     Pure render-throughput benchmark: no modeset, no page
 *                  flip, nothing is scanned out (the console/display is left
 *                  untouched). Reports render rate only. Useful to measure
 *                  raw DE fill-rate above the 60 Hz refresh ceiling.
 *
 * Note: the DE has no implicit bounds checking — an op whose rectangle runs
 * off a surface makes the GPU walk unmapped pages and MMU-fault. Sprite
 * positions are clamped here, and libetna2d also clips every op to the
 * surface as a safety net.
 *
 * Usage:
 *   bench2d [--gpu|--cpu] [-n SPRITES] [-f FRAMES] [--no-vsync]
 *           [--card /dev/dri/card0] [--render /dev/dri/renderD128]
 *
 * Defaults: --gpu, 64 sprites, 300 frames, vsync on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "kms_display.h"
#include "etna2d.h"

#define SPRITE_W 96
#define SPRITE_H 96
#define MAX_SPRITES 256

struct sprite {
	float x, y, vx, vy;
	uint32_t argb;       /* premultiplied */
};

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sprites_init(struct sprite *s, int n, int w, int h)
{
	srand(1234);
	for (int i = 0; i < n; i++) {
		s[i].x  = (float)(rand() % (w - SPRITE_W));
		s[i].y  = (float)(rand() % (h - SPRITE_H));
		s[i].vx = (float)((rand() % 300) - 150) / 60.0f;
		s[i].vy = (float)((rand() % 300) - 150) / 60.0f;
		/* Same premultiplied 50%-red as the GPU sprite texture, so the
		 * CPU and GPU paths composite an identical scene (fair baseline;
		 * only the compositing engine differs). */
		s[i].argb = 0x80800000u;
	}
}

static void sprites_step(struct sprite *s, int n, int w, int h)
{
	for (int i = 0; i < n; i++) {
		s[i].x += s[i].vx;
		s[i].y += s[i].vy;
		/* Reflect AND clamp: reversing velocity alone lets a fast sprite
		 * overshoot the edge for a frame, so (int)y+SPRITE_H could exceed
		 * the surface and read past the BO. Clamp to keep every op fully
		 * in-bounds. */
		if (s[i].x < 0)            { s[i].x = 0;            s[i].vx = -s[i].vx; }
		if (s[i].x > w - SPRITE_W) { s[i].x = w - SPRITE_W; s[i].vx = -s[i].vx; }
		if (s[i].y < 0)            { s[i].y = 0;            s[i].vy = -s[i].vy; }
		if (s[i].y > h - SPRITE_H) { s[i].y = h - SPRITE_H; s[i].vy = -s[i].vy; }
	}
}

/* ---- CPU reference: premultiplied source-over ------------------------ */
static void cpu_blend_sprite(uint32_t *dst, int dstride_px, int dw, int dh,
			     int px, int py, uint32_t src_pm)
{
	int sa = (src_pm >> 24) & 0xff;
	int inv = 255 - sa;
	int sr = (src_pm >> 16) & 0xff;
	int sg = (src_pm >> 8) & 0xff;
	int sb = src_pm & 0xff;
	for (int y = 0; y < SPRITE_H; y++) {
		int dy = py + y;
		if (dy < 0 || dy >= dh) continue;
		uint32_t *row = dst + dy * dstride_px;
		for (int x = 0; x < SPRITE_W; x++) {
			int dx = px + x;
			if (dx < 0 || dx >= dw) continue;
			uint32_t d = row[dx];
			int dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
			int rr = sr + (dr * inv + 127) / 255;
			int rg = sg + (dg * inv + 127) / 255;
			int rb = sb + (db * inv + 127) / 255;
			row[dx] = 0xff000000u | (rr << 16) | (rg << 8) | rb;
		}
	}
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0", *render = "/dev/dri/renderD128";
	int use_gpu = 1, nsprites = 64, frames = 300, vsync = 1;

	for (int i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "--gpu")) use_gpu = 1;
		else if (!strcmp(argv[i], "--cpu")) use_gpu = 0;
		else if (!strcmp(argv[i], "--no-vsync")) vsync = 0;
		else if (!strcmp(argv[i], "-n") && i+1 < argc) nsprites = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-f") && i+1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--card")   && i+1 < argc) card = argv[++i];
		else if (!strcmp(argv[i], "--render") && i+1 < argc) render = argv[++i];
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}
	if (nsprites < 1) nsprites = 1;
	if (nsprites > MAX_SPRITES) nsprites = MAX_SPRITES;

	struct kms_display   *disp = NULL;
	struct etna2d_device *dev  = NULL;
	struct etna2d_context *ctx = NULL;
	struct kms_fb fb[2];
	struct etna2d_surface *surf[2] = { NULL, NULL };   /* GPU: imported scanout */
	struct etna2d_surface *sprite  = NULL;             /* GPU: sprite texture   */
	int dmabuf[2] = { -1, -1 };
	int w, h, rc = 1;
	struct sprite sp[MAX_SPRITES];
	memset(fb, 0, sizeof(fb));

	if (kms_open(card, &disp))
		return 1;
	w = kms_width(disp);
	h = kms_height(disp);

	if (kms_fb_create(disp, &fb[0]) || kms_fb_create(disp, &fb[1]))
		goto out;

	if (use_gpu) {
		if (etna2d_device_open(render, &dev))    goto out;
		if (etna2d_context_create(dev, nsprites + 4, &ctx)) goto out;
		for (int i = 0; i < 2; i++) {
			dmabuf[i] = kms_fb_export_dmabuf(disp, &fb[i]);
			if (dmabuf[i] < 0) goto out;
			if (etna2d_surface_from_dmabuf(dev, dmabuf[i], w, h,
						       fb[i].stride, &surf[i]))
				goto out;
		}
		/* one shared premultiplied sprite texture (solid translucent) */
		if (etna2d_surface_create(dev, SPRITE_W, SPRITE_H, &sprite))
			goto out;
		uint32_t *tx = etna2d_surface_map(sprite);
		for (int i = 0; i < SPRITE_W * SPRITE_H; i++)
			tx[i] = 0x80800000u;   /* placeholder; recolored per-sprite via fill below */
		etna2d_surface_cpu_fini(sprite);
	} else {
		/* CPU mode maps both scanout FBs directly */
		if (!kms_fb_map(disp, &fb[0]) || !kms_fb_map(disp, &fb[1]))
			goto out;
	}

	sprites_init(sp, nsprites, w, h);

	/* Modeset only when we actually present. In --no-vsync mode this is a
	 * pure render-throughput benchmark: nothing is scanned out, so we do
	 * not touch the CRTC at all (leaves the console/display untouched). */
	if (vsync && kms_set_crtc(disp, &fb[0]))
		goto out;

	printf("bench2d: %s  %dx%d  sprites=%d frames=%d vsync=%s\n",
	       use_gpu ? "GPU (GC520 DE)" : "CPU (software)",
	       w, h, nsprites, frames, vsync ? "on" : "off");

	double t0 = now_ms(), twork = 0.0;
	int back = 1;   /* render into fb[back], display fb[!back] */

	for (int f = 0; f < frames; f++) {
		double ws = now_ms();

		if (use_gpu) {
			struct etna2d_surface *d = surf[back];
			etna2d_fill(ctx, d, 0, 0, w, h, 0xff101820u);
			for (int i = 0; i < nsprites; i++)
				etna2d_blend_over(ctx, sprite, d,
						  (int)sp[i].x, (int)sp[i].y,
						  SPRITE_W, SPRITE_H);
			etna2d_finish(ctx);      /* submit + wait GPU fence */
		} else {
			uint32_t *px = fb[back].map;
			int spx = fb[back].stride / 4;
			/* background */
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					px[y * spx + x] = 0xff101820u;
			for (int i = 0; i < nsprites; i++)
				cpu_blend_sprite(px, spx, w, h,
						 (int)sp[i].x, (int)sp[i].y,
						 sp[i].argb);
		}

		twork += now_ms() - ws;

		if (vsync) {
			/* Present each frame and wait the vblank: measures the
			 * real, vsync-capped present rate. */
			kms_page_flip(disp, &fb[back]);
			kms_wait_flip(disp);
			back = !back;
		}
		/* In --no-vsync mode we deliberately do NOT page-flip: this is a
		 * pure render-throughput (fill-rate) benchmark. Flipping without
		 * waiting the previous vblank is rejected with EBUSY and would
		 * drop frames, so presenting is skipped entirely here. FPS is
		 * derived from render time (twork), which for GPU mode is fenced
		 * (real DE throughput) and for CPU mode is the scalar
		 * compositing loop. */

		sprites_step(sp, nsprites, w, h);
	}

	double total = now_ms() - t0;
	double present_fps = frames * 1000.0 / total;
	double render_fps  = frames * 1000.0 / twork;
	printf("\n=== RESULT (%s) ===\n", use_gpu ? "GPU" : "CPU");
	printf("frames      : %d\n", frames);
	printf("render time : %.1f ms total, %.3f ms/frame\n",
	       twork, twork / frames);
	printf("render rate : %.1f FPS (fill-rate, %s)\n", render_fps,
	       use_gpu ? "GPU-fenced" : "CPU loop");
	if (vsync) {
		printf("wall time   : %.1f ms\n", total);
		printf("present rate: %.1f FPS (vsync-capped)\n", present_fps);
	}
	rc = 0;

out:
	for (int i = 0; i < 2; i++) {
		if (dmabuf[i] >= 0) close(dmabuf[i]);
		etna2d_surface_destroy(surf[i]);
	}
	etna2d_surface_destroy(sprite);
	kms_fb_destroy(disp, &fb[0]);
	kms_fb_destroy(disp, &fb[1]);
	etna2d_context_destroy(ctx);
	etna2d_device_close(dev);
	kms_close(disp);
	return rc;
}
