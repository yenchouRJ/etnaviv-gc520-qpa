/*
 * etna2d_util.c — GC520 2D helper layer over libdrm_etnaviv.
 * See etna2d_util.h for the API contract.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xf86drm.h"
#include "etnaviv_drmif.h"
#include "etnaviv_drm.h"   /* ETNA_BO_WC and friends */

#include "state.xml.h"
#include "state_2d.xml.h"
#include "cmdstream.xml.h"

#include "etna2d_util.h"

int etna2d_verbose = 0;

/* ---------------------------------------------------------------------- */
/* Low-level command emit helpers                                         */
/* ---------------------------------------------------------------------- */
static inline void emit_load_state(struct etna_cmd_stream *s,
				   uint16_t offset, uint16_t count)
{
	etna_cmd_stream_emit(s,
		VIV_FE_LOAD_STATE_HEADER_OP_LOAD_STATE |
		VIV_FE_LOAD_STATE_HEADER_OFFSET(offset) |
		(VIV_FE_LOAD_STATE_HEADER_COUNT(count) &
		 VIV_FE_LOAD_STATE_HEADER_COUNT__MASK));
}

/* set a single 32-bit DE state register */
static void ss(struct etna_cmd_stream *s, uint32_t addr, uint32_t val)
{
	etna_cmd_stream_reserve(s, 2);
	emit_load_state(s, addr >> 2, 1);
	etna_cmd_stream_emit(s, val);
}

/* set a DE state register to a BO address (relocation patched by kernel) */
static void ss_reloc(struct etna_cmd_stream *s, uint32_t addr,
		     struct etna_bo *bo, uint32_t flags)
{
	etna_cmd_stream_reserve(s, 2);
	emit_load_state(s, addr >> 2, 1);
	etna_cmd_stream_reloc(s, &(struct etna_reloc){
		.bo = bo,
		.flags = flags,
		.offset = 0,
	});
}

/* Common PE state reset shared by all ops (mirrors the known-good
 * etnaviv_2d_test.c CLEAR sequence). */
static void pe_reset(struct etna_cmd_stream *s)
{
	ss(s, VIVS_DE_SRC_COLOR_BG,          0);
	ss(s, VIVS_DE_SRC_COLOR_FG,          0);
	ss(s, VIVS_DE_DEST_COLOR_KEY,        0);
	ss(s, VIVS_DE_GLOBAL_SRC_COLOR,      0);
	ss(s, VIVS_DE_GLOBAL_DEST_COLOR,     0);
	ss(s, VIVS_DE_COLOR_MULTIPLY_MODES,  0);
	ss(s, VIVS_DE_PE_TRANSPARENCY,       0);
	ss(s, VIVS_DE_PE_CONTROL,            0);
	ss(s, VIVS_DE_PE_DITHER_LOW,         0xffffffff);
	ss(s, VIVS_DE_PE_DITHER_HIGH,        0xffffffff);
}

/* Emit a single DRAW_2D destination rectangle. */
static void draw_2d_rect(struct etna_cmd_stream *s,
			 int x0, int y0, int x1, int y1)
{
	etna_cmd_stream_reserve(s, 4);
	etna_cmd_stream_emit(s,
		VIV_FE_DRAW_2D_HEADER_OP_DRAW_2D |
		VIV_FE_DRAW_2D_HEADER_COUNT(1));
	etna_cmd_stream_emit(s, 0x0); /* rectangles start aligned */
	etna_cmd_stream_emit(s,
		VIV_FE_DRAW_2D_TOP_LEFT_X(x0) | VIV_FE_DRAW_2D_TOP_LEFT_Y(y0));
	etna_cmd_stream_emit(s,
		VIV_FE_DRAW_2D_BOTTOM_RIGHT_X(x1) | VIV_FE_DRAW_2D_BOTTOM_RIGHT_Y(y1));
}

/* Trailing padding + PE2D cache flush (dest becomes coherent for readback). */
static void op_tail(struct etna_cmd_stream *s)
{
	ss(s, 1, 0);
	ss(s, 1, 0);
	ss(s, 1, 0);
	ss(s, VIVS_GL_FLUSH_CACHE, VIVS_GL_FLUSH_CACHE_PE2D);
}

/* Program the source surface for BIT_BLT / STRETCH_BLT. */
static void set_source(struct etna_cmd_stream *s,
		       struct etna_bo *src, uint32_t src_stride,
		       int src_w, int src_h, int src_x, int src_y)
{
	ss(s, VIVS_DE_SRC_STRIDE, src_stride);
	ss(s, VIVS_DE_SRC_ROTATION_CONFIG, 0);
	/* Set both PE1.0 and PE2.0 source-format fields so the op is robust
	 * regardless of which the core samples; they occupy different bits. */
	ss(s, VIVS_DE_SRC_CONFIG,
	   VIVS_DE_SRC_CONFIG_PE10_SOURCE_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_SRC_CONFIG_SOURCE_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_SRC_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_SRC_CONFIG_LOCATION_MEMORY);
	ss(s, VIVS_DE_SRC_ORIGIN,
	   VIVS_DE_SRC_ORIGIN_X(src_x) | VIVS_DE_SRC_ORIGIN_Y(src_y));
	ss(s, VIVS_DE_SRC_SIZE,
	   VIVS_DE_SRC_SIZE_X(src_w) | VIVS_DE_SRC_SIZE_Y(src_h));
	ss_reloc(s, VIVS_DE_SRC_ADDRESS, src, ETNA_RELOC_READ);
}

/* ---------------------------------------------------------------------- */
/* Public: device context                                                 */
/* ---------------------------------------------------------------------- */
int etna2d_open(struct etna2d_ctx *c, const char *path)
{
	drmVersionPtr ver;

	memset(c, 0, sizeof(*c));

	c->fd = open(path, O_RDWR);
	if (c->fd < 0) {
		LOGE("open(%s): %m", path);
		return -1;
	}

	ver = drmGetVersion(c->fd);
	if (ver) {
		LOGI("drm: %s %d.%d.%d (%s)", ver->name,
		     ver->version_major, ver->version_minor,
		     ver->version_patchlevel, ver->desc);
		drmFreeVersion(ver);
	}

	c->dev = etna_device_new(c->fd);
	if (!c->dev) {
		LOGE("etna_device_new: %m");
		goto err_fd;
	}

	/* Scan cores for one advertising 2D support (FEATURES_0 bit 9). */
	c->core = 0;
	for (;;) {
		struct etna_gpu *gpu = etna_gpu_new(c->dev, c->core);
		uint64_t feat;

		if (!gpu) {
			LOGE("no 2D-capable core found (scanned %d)", c->core);
			goto err_dev;
		}
		if (etna_gpu_get_param(gpu, ETNA_GPU_FEATURES_0, &feat)) {
			LOGE("etna_gpu_get_param(FEATURES_0): %m");
			etna_gpu_del(gpu);
			goto err_dev;
		}
		if (feat & (1u << 9)) {
			c->gpu = gpu;
			c->features0 = feat;
			etna_gpu_get_param(gpu, ETNA_GPU_MODEL, &c->model);
			etna_gpu_get_param(gpu, ETNA_GPU_REVISION, &c->revision);
			break;
		}
		LOGD("core %d not 2D-capable (FEATURES_0=0x%08llx)",
		     c->core, (unsigned long long)feat);
		etna_gpu_del(gpu);
		c->core++;
	}

	LOGI("gpu: model GC%03x rev 0x%04x core %d FEATURES_0=0x%08llx",
	     (unsigned)c->model, (unsigned)c->revision, c->core,
	     (unsigned long long)c->features0);

	c->pipe = etna_pipe_new(c->gpu, ETNA_PIPE_2D);
	if (!c->pipe) {
		LOGE("etna_pipe_new(2D): %m");
		goto err_gpu;
	}

	return 0;

err_gpu:
	etna_gpu_del(c->gpu);
	c->gpu = NULL;
err_dev:
	etna_device_del(c->dev);
	c->dev = NULL;
err_fd:
	close(c->fd);
	c->fd = -1;
	return -1;
}

void etna2d_close(struct etna2d_ctx *c)
{
	if (c->pipe) etna_pipe_del(c->pipe);
	if (c->gpu)  etna_gpu_del(c->gpu);
	if (c->dev)  etna_device_del(c->dev);
	if (c->fd >= 0) close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

struct etna_bo *etna2d_bo_new(struct etna2d_ctx *c, uint32_t size,
			      const char *tag)
{
	struct etna_bo *bo = etna_bo_new(c->dev, size, ETNA_BO_WC);
	if (!bo) {
		LOGE("etna_bo_new(%s, %u): %m", tag, size);
		return NULL;
	}
	LOGD("bo %-8s size=%-7u handle=%u map=%p", tag, size,
	     etna_bo_handle(bo), etna_bo_map(bo));
	return bo;
}

/* ---------------------------------------------------------------------- */
/* Public: 2D op builders                                                 */
/* ---------------------------------------------------------------------- */
void etna2d_fill(struct etna_cmd_stream *s,
		 struct etna_bo *dst, uint32_t dst_stride,
		 int x, int y, int w, int h, uint32_t argb)
{
	LOGD("fill  dst rect (%d,%d %dx%d) argb=0x%08x stride=%u",
	     x, y, w, h, argb, dst_stride);

	ss(s, VIVS_DE_SRC_STRIDE, 0);
	ss(s, VIVS_DE_SRC_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_SRC_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN, 0);
	ss(s, VIVS_DE_SRC_SIZE, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, 0);
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst, ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst_stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_CLEAR |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(x) | VIVS_DE_CLIP_TOP_LEFT_Y(y));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(x + w) | VIVS_DE_CLIP_BOTTOM_RIGHT_Y(y + h));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);
	ss(s, VIVS_DE_ALPHA_CONTROL, 0);
	ss(s, VIVS_DE_ALPHA_MODES, 0);
	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	/* PE2.0 clear value (all four slots as in the reference stream) */
	ss(s, VIVS_DE_CLEAR_PIXEL_VALUE32, argb);
	ss(s, VIVS_DE_CLEAR_BYTE_MASK, 0xff);
	ss(s, VIVS_DE_CLEAR_PIXEL_VALUE_LOW, argb);
	ss(s, VIVS_DE_CLEAR_PIXEL_VALUE_HIGH, argb);

	pe_reset(s);
	draw_2d_rect(s, x, y, x + w, y + h);
	op_tail(s);
}

void etna2d_blit(struct etna_cmd_stream *s,
		 struct etna_bo *src, uint32_t src_stride, int src_w, int src_h,
		 int src_x, int src_y,
		 struct etna_bo *dst, uint32_t dst_stride,
		 int dst_x, int dst_y, int w, int h)
{
	LOGD("blit  src (%d,%d) %dx%d stride=%u -> dst (%d,%d) %dx%d stride=%u",
	     src_x, src_y, src_w, src_h, src_stride,
	     dst_x, dst_y, w, h, dst_stride);

	set_source(s, src, src_stride, src_w, src_h, src_x, src_y);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, 0);
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst, ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst_stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);          /* 0xCC = SRCCOPY */
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(dst_x) | VIVS_DE_CLIP_TOP_LEFT_Y(dst_y));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(dst_x + w) |
	   VIVS_DE_CLIP_BOTTOM_RIGHT_Y(dst_y + h));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);
	ss(s, VIVS_DE_ALPHA_CONTROL, 0);    /* blend off */
	ss(s, VIVS_DE_ALPHA_MODES, 0);
	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	pe_reset(s);
	draw_2d_rect(s, dst_x, dst_y, dst_x + w, dst_y + h);
	op_tail(s);
}

void etna2d_blend_over(struct etna_cmd_stream *s,
		 struct etna_bo *src, uint32_t src_stride, int src_w, int src_h,
		 struct etna_bo *dst, uint32_t dst_stride,
		 int dst_x, int dst_y, int w, int h)
{
	LOGD("blend src %dx%d stride=%u -> dst (%d,%d) %dx%d stride=%u (SrcOver)",
	     src_w, src_h, src_stride, dst_x, dst_y, w, h, dst_stride);

	set_source(s, src, src_stride, src_w, src_h, 0, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, 0);
	/* dest is read AND written for blending */
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst, ETNA_RELOC_READ | ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst_stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_BIT_BLT |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(dst_x) | VIVS_DE_CLIP_TOP_LEFT_Y(dst_y));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(dst_x + w) |
	   VIVS_DE_CLIP_BOTTOM_RIGHT_Y(dst_y + h));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);

	/* Porter-Duff "source over", premultiplied source:
	 *   out.rgb = src.rgb + dst.rgb * (1 - src.a)
	 *   out.a   = src.a  + dst.a   * (1 - src.a)
	 *
	 * The GC520 PE operates in premultiplied space.
	 * SRC factor = ONE  (src.rgb is already scaled by src.a; leave it as-is)
	 * DST factor = INVERSED (= 1 - src.a, taken from the src alpha channel)
	 *
	 * Callers MUST provide premultiplied-alpha source pixels, e.g.:
	 *   non-premult (A=0x80, R=0xff) -> premult (A=0x80, R=0x80). */
	ss(s, VIVS_DE_ALPHA_CONTROL, VIVS_DE_ALPHA_CONTROL_ENABLE_ON);
	ss(s, VIVS_DE_ALPHA_MODES,
	   VIVS_DE_ALPHA_MODES_SRC_ALPHA_MODE_NORMAL |
	   VIVS_DE_ALPHA_MODES_DST_ALPHA_MODE_NORMAL |
	   VIVS_DE_ALPHA_MODES_GLOBAL_SRC_ALPHA_MODE_NORMAL |
	   VIVS_DE_ALPHA_MODES_GLOBAL_DST_ALPHA_MODE_NORMAL |
	   VIVS_DE_ALPHA_MODES_SRC_BLENDING_MODE(DE_BLENDMODE_ONE) |
	   VIVS_DE_ALPHA_MODES_DST_BLENDING_MODE(DE_BLENDMODE_INVERSED));

	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	pe_reset(s);
	draw_2d_rect(s, dst_x, dst_y, dst_x + w, dst_y + h);
	op_tail(s);
}

void etna2d_stretch(struct etna_cmd_stream *s,
		 struct etna_bo *src, uint32_t src_stride, int src_w, int src_h,
		 struct etna_bo *dst, uint32_t dst_stride, int dst_w, int dst_h)
{
	/* Vivante stretch factor (.16 fixed point), point sampled:
	 *   factor = ((srcSize - 1) << 16) / (dstSize - 1) */
	uint32_t fx = (dst_w > 1) ? (uint32_t)(((uint64_t)(src_w - 1) << 16) / (dst_w - 1)) : 0;
	uint32_t fy = (dst_h > 1) ? (uint32_t)(((uint64_t)(src_h - 1) << 16) / (dst_h - 1)) : 0;

	LOGD("strch src %dx%d -> dst %dx%d factor x=0x%05x y=0x%05x",
	     src_w, src_h, dst_w, dst_h, fx, fy);

	set_source(s, src, src_stride, src_w, src_h, 0, 0);
	ss(s, VIVS_DE_STRETCH_FACTOR_LOW,  VIVS_DE_STRETCH_FACTOR_LOW_X(fx));
	ss(s, VIVS_DE_STRETCH_FACTOR_HIGH, VIVS_DE_STRETCH_FACTOR_HIGH_Y(fy));
	ss_reloc(s, VIVS_DE_DEST_ADDRESS, dst, ETNA_RELOC_WRITE);
	ss(s, VIVS_DE_DEST_STRIDE, dst_stride);
	ss(s, VIVS_DE_DEST_ROTATION_CONFIG, 0);
	ss(s, VIVS_DE_DEST_CONFIG,
	   VIVS_DE_DEST_CONFIG_FORMAT(DE_FORMAT_A8R8G8B8) |
	   VIVS_DE_DEST_CONFIG_COMMAND_STRETCH_BLT |
	   VIVS_DE_DEST_CONFIG_SWIZZLE(DE_SWIZZLE_ARGB) |
	   VIVS_DE_DEST_CONFIG_TILED_DISABLE |
	   VIVS_DE_DEST_CONFIG_MINOR_TILED_DISABLE);
	ss(s, VIVS_DE_ROP,
	   VIVS_DE_ROP_ROP_FG(0xcc) | VIVS_DE_ROP_ROP_BG(0xcc) |
	   VIVS_DE_ROP_TYPE_ROP4);
	ss(s, VIVS_DE_CLIP_TOP_LEFT,
	   VIVS_DE_CLIP_TOP_LEFT_X(0) | VIVS_DE_CLIP_TOP_LEFT_Y(0));
	ss(s, VIVS_DE_CLIP_BOTTOM_RIGHT,
	   VIVS_DE_CLIP_BOTTOM_RIGHT_X(dst_w) | VIVS_DE_CLIP_BOTTOM_RIGHT_Y(dst_h));
	ss(s, VIVS_DE_CONFIG, 0);
	ss(s, VIVS_DE_SRC_ORIGIN_FRACTION, 0);
	ss(s, VIVS_DE_ALPHA_CONTROL, 0);
	ss(s, VIVS_DE_ALPHA_MODES, 0);
	ss(s, VIVS_DE_DEST_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_SRC_ROTATION_HEIGHT, 0);
	ss(s, VIVS_DE_ROT_ANGLE, 0);

	pe_reset(s);
	draw_2d_rect(s, 0, 0, dst_w, dst_h);
	op_tail(s);
}

/* ---------------------------------------------------------------------- */
/* Public: verification helpers                                           */
/* ---------------------------------------------------------------------- */
int etna2d_cmp_exact(const uint32_t *got, const uint32_t *expect, int n,
		     const char *tag)
{
	for (int i = 0; i < n; i++) {
		if (got[i] != expect[i]) {
			LOGE("%s: pixel %d (x=%d) expected 0x%08x got 0x%08x",
			     tag, i, i, expect[i], got[i]);
			return -1;
		}
	}
	return 0;
}

int etna2d_check_px(const uint32_t *buf, int stride_px, int x, int y,
		    uint32_t expect, const char *tag)
{
	uint32_t got = buf[y * stride_px + x];
	if (got != expect) {
		LOGE("%s: (%d,%d) expected 0x%08x got 0x%08x",
		     tag, x, y, expect, got);
		return -1;
	}
	LOGD("%s: (%d,%d) = 0x%08x OK", tag, x, y, got);
	return 0;
}

int etna2d_cmp_tol(uint32_t got, uint32_t expect, int tol)
{
	for (int shift = 0; shift < 32; shift += 8) {
		int g = (got    >> shift) & 0xff;
		int e = (expect >> shift) & 0xff;
		int d = g - e;
		if (d < 0) d = -d;
		if (d > tol)
			return -1;
	}
	return 0;
}
