/*
 * bench_stretch.c — Phase 3: GC520 scaler (stretch-blit) benchmark.
 *
 * Companion to bench2d. Where bench2d stresses the compositing path
 * (fill + SrcOver blend), this stresses the DE's *scaler*: point-sampled
 * StretchBlt from one fixed source texture into destination rectangles of a
 * chosen scale, tiled to cover the screen. Scaler cost is dominated by the
 * DESTINATION area (each output pixel samples the source), so we report
 * destination Mpixels/sec, and sweep a range of scale factors because up-
 * and down-scaling stress the sampler and memory system differently.
 *
 * Two modes draw the SAME scaled scene so only the engine differs:
 *   --gpu   GC520 DE via etna2d_stretch_batch(): all tiles of a frame are
 *           issued as ONE op-batch that emits the heavy DE state once, so
 *           per-tile cost is just clip+draw (crucial for tiny-tile frames)
 *   --cpu   software point-sampled scaler (nearest neighbour, same as HW)
 *
 * For each scale the source (SRC_W x SRC_H) is stretched to
 * (SRC_W*scale x SRC_H*scale) and tiled across the whole framebuffer; the
 * count of instances is whatever fills the screen. Instances that overhang
 * the edges are fine — libetna2d clips every op to the surface, so no
 * hand-clamping is needed (the DE has no implicit bounds check).
 *
 * Presentation mirrors bench2d:
 *   default      double-buffered, page-flip + vblank; reports render rate
 *                (GPU-fenced fill-rate) and present rate (vsync-capped)
 *   --no-vsync   no modeset / no present; pure render-throughput. Best for
 *                reading raw scaler fill-rate above the 60 Hz ceiling.
 *
 * Usage:
 *   bench_stretch [--gpu|--cpu] [-f FRAMES] [--no-vsync]
 *                 [--scale F | --sweep] [--src WxH]
 *                 [--card /dev/dri/card0] [--render /dev/dri/renderD128]
 *
 * Defaults: --gpu, 120 frames, --sweep (0.25,0.5,1,2,4), 128x128 source.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "kms_display.h"
#include "etna2d.h"

#define MAX_SCALES 8

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Fill the source with a distinct 4-quadrant colour pattern so scaling is
 * visually verifiable and every source texel is a real colour. */
static void source_pattern(uint32_t *px, int sw, int sh)
{
	static const uint32_t col[4] = {
		0xffff2020u, 0xff20ff20u, 0xff2020ffu, 0xfff0f0f0u
	};
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < sw; x++) {
			int q = (x >= sw / 2) | ((y >= sh / 2) << 1);
			px[y * sw + x] = col[q];
		}
}

/* CPU reference: nearest-neighbour scale of src(sw x sh) into dst rect at
 * (dx,dy) of size (dw x dh). Uses the same ((size-1)<<16)/(dst-1) mapping as
 * the DE so the two engines produce identical pixels. Clipped to the dest. */
static void cpu_stretch(uint32_t *dst, int dstride_px, int dstw, int dsth,
			const uint32_t *src, int sw, int sh,
			int dx, int dy, int dw, int dh)
{
	uint32_t fx = (dw > 1) ? (uint32_t)(((uint64_t)(sw - 1) << 16) / (dw - 1)) : 0;
	uint32_t fy = (dh > 1) ? (uint32_t)(((uint64_t)(sh - 1) << 16) / (dh - 1)) : 0;
	for (int j = 0; j < dh; j++) {
		int ry = dy + j;
		if (ry < 0 || ry >= dsth) continue;
		int syi = (int)(((uint64_t)j * fy) >> 16);
		const uint32_t *srow = src + syi * sw;
		uint32_t *drow = dst + ry * dstride_px;
		for (int i = 0; i < dw; i++) {
			int rx = dx + i;
			if (rx < 0 || rx >= dstw) continue;
			int sxi = (int)(((uint64_t)i * fx) >> 16);
			drow[rx] = srow[sxi];
		}
	}
}

/* Draw one full frame of tiled stretched instances at the given scale into
 * back buffer `back`. Returns the number of destination pixels touched
 * (in-bounds) so the caller can compute a fill-rate. */
static long draw_frame(int use_gpu, double scale,
		       struct etna2d_context *ctx,
		       struct etna2d_surface *gpu_dst,
		       struct etna2d_surface *gpu_src,
		       uint32_t *cpu_dst, int cpu_stride_px,
		       const uint32_t *cpu_src,
		       int sw, int sh, int w, int h)
{
	int dw = (int)(sw * scale + 0.5);
	int dh = (int)(sh * scale + 0.5);
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;

	long touched = 0;
	if (use_gpu)
		etna2d_fill(ctx, gpu_dst, 0, 0, w, h, 0xff000000u);
	else
		for (int y = 0; y < h; y++)
			memset(cpu_dst + y * cpu_stride_px, 0, (size_t)w * 4);

	if (use_gpu) {
		/* Batched stretch: build the tile list, then issue all tiles as
		 * one op-batch that emits the heavy DE state only once (instead
		 * of per tile). This is the big win for tiny-tile / heavy-tile
		 * frames where fixed per-op cost dominates. */
		static struct etna2d_rect *rects = NULL;
		static int cap = 0;
		int ntiles = ((w + dw - 1) / dw) * ((h + dh - 1) / dh);
		if (ntiles > cap) {
			free(rects);
			rects = malloc((size_t)ntiles * sizeof(*rects));
			cap = ntiles;
		}
		int n = 0;
		for (int dy = 0; dy < h; dy += dh) {
			for (int dx = 0; dx < w; dx += dw) {
				rects[n].x = dx; rects[n].y = dy;
				rects[n].w = dw; rects[n].h = dh;
				n++;
				int cw = (dx + dw > w) ? (w - dx) : dw;
				int ch = (dy + dh > h) ? (h - dy) : dh;
				touched += (long)cw * ch;
			}
		}
		etna2d_stretch_batch(ctx, gpu_src, gpu_dst, rects, n);
		etna2d_finish(ctx);   /* submit + wait GPU fence */
	} else {
		for (int dy = 0; dy < h; dy += dh) {
			for (int dx = 0; dx < w; dx += dw) {
				cpu_stretch(cpu_dst, cpu_stride_px, w, h,
					    cpu_src, sw, sh, dx, dy, dw, dh);
				int cw = (dx + dw > w) ? (w - dx) : dw;
				int ch = (dy + dh > h) ? (h - dy) : dh;
				touched += (long)cw * ch;
			}
		}
	}
	return touched;
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0", *render = "/dev/dri/renderD128";
	int use_gpu = 1, frames = 120, vsync = 1;
	int sweep = 1, sw = 128, sh = 128;
	double scales[MAX_SCALES] = { 0.25, 0.5, 1.0, 2.0, 4.0 };
	int nscales = 5;

	for (int i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "--gpu")) use_gpu = 1;
		else if (!strcmp(argv[i], "--cpu")) use_gpu = 0;
		else if (!strcmp(argv[i], "--no-vsync")) vsync = 0;
		else if (!strcmp(argv[i], "--sweep")) sweep = 1;
		else if (!strcmp(argv[i], "-f") && i+1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--scale") && i+1 < argc) {
			scales[0] = atof(argv[++i]); nscales = 1; sweep = 0;
		}
		else if (!strcmp(argv[i], "--src") && i+1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &sw, &sh) != 2 || sw < 1 || sh < 1) {
				fprintf(stderr, "bad --src (want WxH)\n"); return 2;
			}
		}
		else if (!strcmp(argv[i], "--card")   && i+1 < argc) card = argv[++i];
		else if (!strcmp(argv[i], "--render") && i+1 < argc) render = argv[++i];
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
	}
	if (frames < 1) frames = 1;
	(void)sweep;

	struct kms_display   *disp = NULL;
	struct etna2d_device *dev  = NULL;
	struct etna2d_context *ctx = NULL;
	struct kms_fb fb[2];
	struct etna2d_surface *surf[2] = { NULL, NULL };  /* GPU: imported scanout */
	struct etna2d_surface *gsrc    = NULL;            /* GPU: source texture   */
	uint32_t *csrc = NULL;                            /* CPU: source texture   */
	int dmabuf[2] = { -1, -1 };
	int w, h, rc = 1;
	memset(fb, 0, sizeof(fb));

	if (kms_open(card, &disp))
		return 1;
	w = kms_width(disp);
	h = kms_height(disp);

	if (kms_fb_create(disp, &fb[0]) || kms_fb_create(disp, &fb[1]))
		goto out;

	if (use_gpu) {
		if (etna2d_device_open(render, &dev))    goto out;
		/* The stretch batch API flushes internally to respect the
		 * kernel per-submit cap, so the context only needs to hold one
		 * submit's worth of tiles, not the whole frame. A modest buffer
		 * is plenty; oversizing just wastes memory. */
		if (etna2d_context_create(dev, 1024, &ctx)) goto out;
		for (int i = 0; i < 2; i++) {
			dmabuf[i] = kms_fb_export_dmabuf(disp, &fb[i]);
			if (dmabuf[i] < 0) goto out;
			if (etna2d_surface_from_dmabuf(dev, dmabuf[i], w, h,
						       fb[i].stride, &surf[i]))
				goto out;
		}
		if (etna2d_surface_create(dev, sw, sh, &gsrc)) goto out;
		uint32_t *tx = etna2d_surface_map(gsrc);
		source_pattern(tx, sw, sh);
		etna2d_surface_cpu_fini(gsrc);
	} else {
		if (!kms_fb_map(disp, &fb[0]) || !kms_fb_map(disp, &fb[1]))
			goto out;
		csrc = malloc((size_t)sw * sh * 4);
		if (!csrc) goto out;
		source_pattern(csrc, sw, sh);
	}

	if (vsync && kms_set_crtc(disp, &fb[0]))
		goto out;

	printf("bench_stretch: %s  screen=%dx%d  src=%dx%d  frames=%d  vsync=%s\n",
	       use_gpu ? "GPU (GC520 DE scaler)" : "CPU (nearest-neighbour)",
	       w, h, sw, sh, frames, vsync ? "on" : "off");
	printf("%-8s %-9s %-12s %-14s %-14s\n",
	       "scale", "tile", "ms/frame", "render Mpx/s", vsync ? "present FPS" : "");

	int back = 1;
	for (int si = 0; si < nscales; si++) {
		double scale = scales[si];
		int dw = (int)(sw * scale + 0.5), dh = (int)(sh * scale + 0.5);
		if (dw < 1) dw = 1;
		if (dh < 1) dh = 1;

		double twork = 0.0, t0 = now_ms();
		long touched = 0;

		for (int f = 0; f < frames; f++) {
			double ws = now_ms();
			touched = draw_frame(use_gpu, scale, ctx,
					     surf[back], gsrc,
					     use_gpu ? NULL : fb[back].map,
					     use_gpu ? 0 : fb[back].stride / 4,
					     csrc, sw, sh, w, h);
			twork += now_ms() - ws;

			if (vsync) {
				kms_page_flip(disp, &fb[back]);
				kms_wait_flip(disp);
				back = !back;
			}
		}

		double total = now_ms() - t0;
		double msf = twork / frames;
		double mpx = (touched / (msf / 1000.0)) / 1e6;
		char tile[24], scal[16];
		snprintf(tile, sizeof(tile), "%dx%d", dw, dh);
		snprintf(scal, sizeof(scal), "%.2fx", scale);
		if (vsync)
			printf("%-8s %-9s %-12.3f %-14.1f %-14.1f\n",
			       scal, tile, msf, mpx, frames * 1000.0 / total);
		else
			printf("%-8s %-9s %-12.3f %-14.1f\n",
			       scal, tile, msf, mpx);
	}
	rc = 0;

out:
	for (int i = 0; i < 2; i++) {
		if (dmabuf[i] >= 0) close(dmabuf[i]);
		etna2d_surface_destroy(surf[i]);
	}
	free(csrc);
	etna2d_surface_destroy(gsrc);
	kms_fb_destroy(disp, &fb[0]);
	kms_fb_destroy(disp, &fb[1]);
	etna2d_context_destroy(ctx);
	etna2d_device_close(dev);
	kms_close(disp);
	return rc;
}
