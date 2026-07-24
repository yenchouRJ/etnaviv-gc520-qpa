/*
 * etna2d_util.h — minimal GC520 2D helper layer over libdrm_etnaviv.
 *
 * This is the seed of the future "libetna2d" userspace 2D builder
 * (SUMMARY.md Phase 2). Each op builder emits a complete, self-contained
 * DE (drawing engine) command sequence into an etna_cmd_stream: full state
 * setup + a DRAW_2D rectangle. Callers just build one op then finish().
 *
 * Target: Vivante GC520 (2D-only, PE 2.0), ARGB8888 surfaces.
 */
#ifndef ETNA2D_UTIL_H
#define ETNA2D_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include "etnaviv_drmif.h"

/* ---------------------------------------------------------------------- */
/* Debug logging                                                          */
/* ---------------------------------------------------------------------- */
extern int etna2d_verbose;   /* set from ETNA2D_VERBOSE env or -v */

#define LOGI(...)  do { fprintf(stderr, "[INFO] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGW(...)  do { fprintf(stderr, "[WARN] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGE(...)  do { fprintf(stderr, "[FAIL] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGOK(...) do { fprintf(stderr, "[ OK ] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGD(...)  do { if (etna2d_verbose) { fprintf(stderr, "[DBG ] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------------------------------------------------------------------- */
/* Device context                                                         */
/* ---------------------------------------------------------------------- */
struct etna2d_ctx {
	int fd;
	struct etna_device *dev;
	struct etna_gpu    *gpu;
	struct etna_pipe   *pipe;
	int      core;          /* core index that carries the 2D pipe */
	uint64_t model;
	uint64_t revision;
	uint64_t features0;
};

/* Open the DRM render node, find a 2D-capable core, create the 2D pipe.
 * Returns 0 on success. Prints identifying info at INFO level. */
int  etna2d_open(struct etna2d_ctx *c, const char *path);
void etna2d_close(struct etna2d_ctx *c);

/* Allocate a write-combine BO (ETNA_BO_WC — ordinary memory, safe for
 * memset/CPU fill; see SUMMARY.md note on UNCACHED vs WC). */
struct etna_bo *etna2d_bo_new(struct etna2d_ctx *c, uint32_t size, const char *tag);

/* ---------------------------------------------------------------------- */
/* 2D op builders — each emits ONE complete self-contained DE operation.  */
/* All surfaces are DE_FORMAT_A8R8G8B8 / DE_SWIZZLE_ARGB.                  */
/* strides are in BYTES; coordinates/sizes in PIXELS.                     */
/* ---------------------------------------------------------------------- */

/* Solid fill (DE CLEAR) of a dest rectangle with a 0xAARRGGBB value. */
void etna2d_fill(struct etna_cmd_stream *s,
		 struct etna_bo *dst, uint32_t dst_stride,
		 int x, int y, int w, int h, uint32_t argb);

/* Opaque copy (DE BIT_BLT, ROP4 SRCCOPY 0xCC): src rect -> dst rect, 1:1. */
void etna2d_blit(struct etna_cmd_stream *s,
		 struct etna_bo *src, uint32_t src_stride, int src_w, int src_h,
		 int src_x, int src_y,
		 struct etna_bo *dst, uint32_t dst_stride,
		 int dst_x, int dst_y, int w, int h);

/* Alpha blend (BIT_BLT + PE), premultiplied "source over":
 *   out.rgb = src.rgb + dst.rgb * (1 - src.a)
 * The GC520 PE operates in premultiplied space. Callers MUST provide
 * premultiplied-alpha pixels in the source BO (src.rgb already scaled by
 * src.a). Qt renders premultiplied internally so this matches the paint
 * engine path directly. src origin is (0,0). */
void etna2d_blend_over(struct etna_cmd_stream *s,
		 struct etna_bo *src, uint32_t src_stride, int src_w, int src_h,
		 struct etna_bo *dst, uint32_t dst_stride,
		 int dst_x, int dst_y, int w, int h);

/* Scaled copy (DE STRETCH_BLT, point sampled): full src -> full dst. */
void etna2d_stretch(struct etna_cmd_stream *s,
		 struct etna_bo *src, uint32_t src_stride, int src_w, int src_h,
		 struct etna_bo *dst, uint32_t dst_stride, int dst_w, int dst_h);

/* ---------------------------------------------------------------------- */
/* Pixel verification helpers                                             */
/* ---------------------------------------------------------------------- */

/* Exact compare of n ARGB pixels. Returns 0 on match, -1 on first diff. */
int etna2d_cmp_exact(const uint32_t *got, const uint32_t *expect, int n,
		     const char *tag);

/* Check a single pixel at (x,y) equals expect exactly. Returns 0/-1. */
int etna2d_check_px(const uint32_t *buf, int stride_px, int x, int y,
		    uint32_t expect, const char *tag);

/* Per-channel compare with +/- tol tolerance (for HW blend rounding). */
int etna2d_cmp_tol(uint32_t got, uint32_t expect, int tol);

#endif /* ETNA2D_UTIL_H */
