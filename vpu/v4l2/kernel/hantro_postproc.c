// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro G1 post-processor support
 *
 * Copyright (C) 2019 Collabora, Ltd.
 */

#include <linux/dma-mapping.h>
#include <linux/types.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_g1_regs.h"
#include "hantro_g2_regs.h"
#include "hantro_v4l2.h"

#define HANTRO_PP_REG_WRITE(vpu, reg_name, val) \
{ \
	hantro_reg_write(vpu, \
			 &hantro_g1_postproc_regs.reg_name, \
			 val); \
}

#define HANTRO_PP_REG_WRITE_RELAXED(vpu, reg_name, val) \
{ \
	hantro_reg_write_relaxed(vpu, \
				 &hantro_g1_postproc_regs.reg_name, \
				 val); \
}

#define VPU_PP_IN_YUYV			0x0
#define VPU_PP_IN_NV12			0x1
#define VPU_PP_IN_YUV420		0x2
#define VPU_PP_IN_YUV240_TILED		0x5
#define VPU_PP_OUT_RGB			0x0
#define VPU_PP_OUT_YUYV			0x3
#define VPU_PP_OUT_NV12			0x5	/* YCh 4:2:0 chroma interleaved */

static const struct hantro_postproc_regs hantro_g1_postproc_regs = {
	.pipeline_en = {G1_REG_PP_INTERRUPT, 1, 0x1},
	.max_burst = {G1_REG_PP_DEV_CONFIG, 0, 0x1f},
	.clk_gate = {G1_REG_PP_DEV_CONFIG, 1, 0x1},
	.out_swap32 = {G1_REG_PP_DEV_CONFIG, 5, 0x1},
	.out_endian = {G1_REG_PP_DEV_CONFIG, 6, 0x1},
	.out_luma_base = {G1_REG_PP_OUT_LUMA_BASE, 0, 0xffffffff},
	.out_chroma_base = {G1_REG_PP_OUT_CHROMA_BASE, 0, 0xffffffff},
	.input_width = {G1_REG_PP_INPUT_SIZE, 0, 0x1ff},
	/* swreg72 bits 16:9: 8 bits (bit 17 unused); overflow goes to input_height_ext */
	.input_height = {G1_REG_PP_INPUT_SIZE, 9, 0xff},
	.output_width = {G1_REG_PP_CONTROL, 4, 0x7ff},
	.output_height = {G1_REG_PP_CONTROL, 15, 0x7ff},
	.input_fmt = {G1_REG_PP_CONTROL, 29, 0x7},
	.output_fmt = {G1_REG_PP_CONTROL, 26, 0x7},
	.orig_width = {G1_REG_PP_MASK1_ORIG_WIDTH, 23, 0x1ff},
	/* swreg92 bits 12:0: 13 bits so a 4096 wide output fits (mainline: 0xfff) */
	.display_width = {G1_REG_PP_DISPLAY_WIDTH_IN_EXT, 0, 0x1fff},
	.input_width_ext = {G1_REG_PP_DISPLAY_WIDTH_IN_EXT, 26, 0x7},
	/* input crop origin: vendor table 8170table.h (STARTX swreg71 [29:21], STARTY swreg72 [31:24]) */
	.crop_startx = {G1_REG_PP_COLOR_CONVERSION1, 21, 0x1ff},
	.crop_starty = {G1_REG_PP_INPUT_SIZE, 24, 0xff},
	.crop_startx_ext = {G1_REG_PP_DISPLAY_WIDTH_IN_EXT, 20, 0x7},
	.crop_starty_ext = {G1_REG_PP_DISPLAY_WIDTH_IN_EXT, 23, 0x7},
	/* swreg85 bit 1 / bit 0: right / bottom crop not a multiple of 16 */
	.crop8_r_e = {G1_REG_PP_CONTROL, 1, 0x1},
	.crop8_d_e = {G1_REG_PP_CONTROL, 0, 0x1},
	.input_height_ext = {G1_REG_PP_DISPLAY_WIDTH_IN_EXT, 29, 0x7},
	/* output size bits 12:11 (swreg70 / swreg71 bits 31:30) */
	.output_height_ext = {G1_REG_PP_COLOR_CONVERSION0, 30, 0x3},
	.output_width_ext = {G1_REG_PP_COLOR_CONVERSION1, 30, 0x3},
	.hor_scale_mode = {G1_REG_PP_SCALING1, 25, 0x3},
	.ver_scale_mode = {G1_REG_PP_SCALING1, 23, 0x3},
	.scale_wratio = {G1_REG_PP_SCALING0, 0, 0x3ffff},
	.scale_hratio = {G1_REG_PP_SCALING1, 0, 0x3ffff},
	.wscale_invra = {G1_REG_PP_SCALING2, 16, 0xffff},
	.hscale_invra = {G1_REG_PP_SCALING2, 0, 0xffff},
	.out_swap16 = {G1_REG_PP_CONTROL, 2, 0x1},
	.rgb_pix_in32 = {G1_REG_PP_SCALING0, 28, 0x1},
	.ycbcr_range = {G1_REG_PP_SCALING0, 29, 0x1},
	.rgb_r_padd = {G1_REG_PP_SCALING0, 23, 0x1f},
	.rgb_g_padd = {G1_REG_PP_SCALING0, 18, 0x1f},
	.rgb_b_padd = {G1_REG_PP_SCALING1, 18, 0x1f},
	.r_mask = {G1_REG_PP_MASK_R, 0, 0xffffffff},
	.g_mask = {G1_REG_PP_MASK_G, 0, 0xffffffff},
	.b_mask = {G1_REG_PP_MASK_B, 0, 0xffffffff},
	.contrast_thr1 = {G1_REG_PP_CONTRAST_ADJUST, 24, 0xff},
	.contrast_off2 = {G1_REG_PP_CONTRAST_ADJUST, 10, 0x3ff},
	.contrast_off1 = {G1_REG_PP_CONTRAST_ADJUST, 0, 0x3ff},
	.coeff_a2 = {G1_REG_PP_COLOR_CONVERSION, 18, 0x3ff},
	.coeff_a1 = {G1_REG_PP_COLOR_CONVERSION, 8, 0x3ff},
	.contrast_thr2 = {G1_REG_PP_COLOR_CONVERSION, 0, 0xff},
	.coeff_d = {G1_REG_PP_COLOR_CONVERSION0, 20, 0x3ff},
	.coeff_c = {G1_REG_PP_COLOR_CONVERSION0, 10, 0x3ff},
	.coeff_b = {G1_REG_PP_COLOR_CONVERSION0, 0, 0x3ff},
	.coeff_f = {G1_REG_PP_COLOR_CONVERSION1, 10, 0xff},
	.coeff_e = {G1_REG_PP_COLOR_CONVERSION1, 0, 0x3ff},
};

#define PP_SCALE_OFF	0
#define PP_SCALE_UP	1
#define PP_SCALE_DOWN	2

bool hantro_postproc_forced(const struct hantro_ctx *ctx)
{
	const struct hantro_variant *v = ctx->dev->variant;

	return !ctx->is_encoder && v->force_postproc && *v->force_postproc;
}

/*
 * Validate a requested CAPTURE size against the PP scaler limits (HW Integration Guide
 * 2.3 / vendor ppinternal.c): width step 8, height step 2, 16..4096, up-scale at most 3x
 * (3x - 2 for the height), no up-scale on one axis together with a down-scale on the
 * other.  Returns true when the PP has to scale; false (size reset to the coded size) when
 * the request equals the coded size, e.g. the visible 1080 of a coded 1088.  pp_only (the
 * CAPTURE format exists only through the PP, e.g. RGB) keeps such a request and always
 * reports true.
 */
bool hantro_postproc_try_size(const struct hantro_ctx *ctx, u32 *width, u32 *height,
			      bool pp_only)
{
	u32 sw = ctx->src_fmt.width, sh = ctx->src_fmt.height;
	u32 vw = sw, vh = sh;
	u32 w = *width, h = *height;

	/* The scaler reads the visible part of the picture when the SPS crops it */
	if (ctx->crop_w && ctx->crop_h && ALIGN(ctx->crop_w, MB_DIM) == sw &&
	    ALIGN(ctx->crop_h, MB_DIM) == sh) {
		vw = ctx->crop_w;
		vh = ctx->crop_h;
	}

	if (!w || !h || (!pp_only && ALIGN(w, MB_DIM) == sw && ALIGN(h, MB_DIM) == sh)) {
		*width = sw;
		*height = sh;
		return false;
	}

	w = ALIGN(clamp(w, 16U, min(4096U, 3 * vw)), 8);
	h = ALIGN(clamp(h, 16U, min(4096U, 3 * vh - 2)), 2);
	w = min(w, min(4096U, 3 * vw) & ~7U);
	h = min(h, min(4096U, 3 * vh - 2) & ~1U);

	/* Mixed up/down scaling is not possible: the up-scaled axis stays at the source size */
	if (w > vw && h < vh)
		w = vw;
	else if (h > vh && w < vw)
		h = vh;

	*width = w;
	*height = h;
	return pp_only || w != sw || h != sh;
}

/*
 * PP input crop: true (and the cropped size) when the PP scales a picture whose SPS crop
 * is usable.  Without scaling the whole coded picture is passed through.
 */
bool hantro_postproc_crop(const struct hantro_ctx *ctx, u32 *w, u32 *h)
{
	*w = ctx->ref_fmt.width;
	*h = ctx->ref_fmt.height;

	if (!ctx->crop_w || !ctx->crop_h ||
	    ALIGN(ctx->crop_w, MB_DIM) != *w || ALIGN(ctx->crop_h, MB_DIM) != *h ||
	    (ctx->dst_fmt.width == *w && ctx->dst_fmt.height == *h))
		return false;

	*w = ctx->crop_w;
	*h = ctx->crop_h;
	return true;
}

static void hantro_postproc_g1_scale(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	u32 iw, ih;
	u32 ow = ctx->dst_fmt.width, oh = ctx->dst_fmt.height;

	hantro_postproc_crop(ctx, &iw, &ih);

	/* Ratios as in the vendor PP driver (ppinternal.c), 16-bit fixed point */
	if (iw < ow) {
		HANTRO_PP_REG_WRITE(vpu, hor_scale_mode, PP_SCALE_UP);
		HANTRO_PP_REG_WRITE(vpu, scale_wratio, ((ow - 1) << 16) / (iw - 1));
		HANTRO_PP_REG_WRITE(vpu, wscale_invra, ((iw - 1) << 16) / (ow - 1));
	} else if (iw > ow) {
		HANTRO_PP_REG_WRITE(vpu, hor_scale_mode, PP_SCALE_DOWN);
		HANTRO_PP_REG_WRITE(vpu, wscale_invra, (ow << 16) / iw);
	} else {
		HANTRO_PP_REG_WRITE(vpu, hor_scale_mode, PP_SCALE_OFF);
		HANTRO_PP_REG_WRITE(vpu, wscale_invra, 0);
	}

	if (ih < oh) {
		HANTRO_PP_REG_WRITE(vpu, ver_scale_mode, PP_SCALE_UP);
		HANTRO_PP_REG_WRITE(vpu, scale_hratio, ((oh - 1) << 16) / (ih - 1));
		HANTRO_PP_REG_WRITE(vpu, hscale_invra, ((ih - 1) << 16) / (oh - 1));
	} else if (ih > oh) {
		HANTRO_PP_REG_WRITE(vpu, ver_scale_mode, PP_SCALE_DOWN);
		HANTRO_PP_REG_WRITE(vpu, hscale_invra, ((oh << 16) / ih) + 1);
	} else {
		HANTRO_PP_REG_WRITE(vpu, ver_scale_mode, PP_SCALE_OFF);
		HANTRO_PP_REG_WRITE(vpu, hscale_invra, 0);
	}
}

static void hantro_postproc_g1_crop(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	u32 w, h;
	bool crop = hantro_postproc_crop(ctx, &w, &h);

	/* Input size in MBs is the cropped one; the crop origin is always 0 */
	HANTRO_PP_REG_WRITE(vpu, input_width, MB_WIDTH(w));
	HANTRO_PP_REG_WRITE(vpu, input_height, MB_HEIGHT(h));
	HANTRO_PP_REG_WRITE(vpu, input_width_ext, MB_WIDTH(w) >> 9);
	HANTRO_PP_REG_WRITE(vpu, input_height_ext, MB_HEIGHT(h) >> 8);
	HANTRO_PP_REG_WRITE(vpu, crop_startx, 0);
	HANTRO_PP_REG_WRITE(vpu, crop_starty, 0);
	HANTRO_PP_REG_WRITE(vpu, crop_startx_ext, 0);
	HANTRO_PP_REG_WRITE(vpu, crop_starty_ext, 0);
	HANTRO_PP_REG_WRITE(vpu, crop8_r_e, crop && (w & 15));
	HANTRO_PP_REG_WRITE(vpu, crop8_d_e, crop && (h & 15));
}

static int hantro_postproc_g1_enum_framesizes(struct hantro_ctx *ctx,
					      struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index || !ctx->src_fmt.width || !ctx->src_fmt.height)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 16;
	fsize->stepwise.min_height = 16;
	fsize->stepwise.max_width = min(4096U, 3 * (ctx->crop_w ?: ctx->src_fmt.width));
	fsize->stepwise.max_height = min(4096U, 3 * (ctx->crop_h ?: ctx->src_fmt.height) - 2);
	fsize->stepwise.step_width = 8;
	fsize->stepwise.step_height = 2;

	return 0;
}

bool hantro_needs_postproc(const struct hantro_ctx *ctx,
			   const struct hantro_fmt *fmt)
{
	if (ctx->is_encoder)
		return false;

	if (ctx->need_postproc)
		return true;

	return fmt->postprocessed;
}

bool hantro_postproc_is_rgb(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_XRGB32 || fourcc == V4L2_PIX_FMT_RGB565;
}

/*
 * RGB output: channel masks/pad positions (vendor PPSetRgbBitmask) and the YCbCr to RGB
 * matrix (vendor PPSetRgbTransformCoeffs, ppapi.c tables). Matrix and range follow the
 * CAPTURE colorimetry: BT.709 only when ycbcr_enc/colorspace say so, full range only when
 * quantization/colorspace say so (default: BT.601, limited).
 */
static void hantro_postproc_g1_rgb(struct hantro_ctx *ctx, bool rgb16)
{
	struct hantro_dev *vpu = ctx->dev;
	const struct v4l2_pix_format_mplane *f = &ctx->dst_fmt;
	u32 enc = f->ycbcr_enc, quant = f->quantization;
	bool bt709, full;
	u32 a, b, c, d, e;

	if (enc == V4L2_YCBCR_ENC_DEFAULT)
		bt709 = f->colorspace == V4L2_COLORSPACE_REC709;
	else
		bt709 = enc == V4L2_YCBCR_ENC_709;
	if (quant == V4L2_QUANTIZATION_DEFAULT)
		/* the reset default colorspace is JPEG, so only a JPEG stream means full range */
		full = ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_JPEG;
	else
		full = quant == V4L2_QUANTIZATION_FULL_RANGE;

	if (!bt709 && !full) {
		a = 298; b = 409; c = 208; d = 100; e = 516;
	} else if (!bt709) {
		a = 256; b = 350; c = 179; d = 86; e = 443;
	} else if (!full) {
		a = 298; b = 459; c = 137; d = 55; e = 544;
	} else {
		a = 256; b = 403; c = 120; d = 48; e = 475;
	}

	HANTRO_PP_REG_WRITE(vpu, ycbcr_range, full);
	HANTRO_PP_REG_WRITE(vpu, rgb_pix_in32, rgb16);
	/* No contrast, brightness or saturation adjustment (saturation 64 = x1) */
	HANTRO_PP_REG_WRITE(vpu, contrast_thr1, 55);
	HANTRO_PP_REG_WRITE(vpu, contrast_thr2, 165);
	HANTRO_PP_REG_WRITE(vpu, contrast_off1, 0);
	HANTRO_PP_REG_WRITE(vpu, contrast_off2, 0);
	HANTRO_PP_REG_WRITE(vpu, coeff_a1, a);
	HANTRO_PP_REG_WRITE(vpu, coeff_a2, a);
	HANTRO_PP_REG_WRITE(vpu, coeff_b, b);
	HANTRO_PP_REG_WRITE(vpu, coeff_c, c);
	HANTRO_PP_REG_WRITE(vpu, coeff_d, d);
	HANTRO_PP_REG_WRITE(vpu, coeff_e, e);
	HANTRO_PP_REG_WRITE(vpu, coeff_f, 0);

	if (rgb16) {		/* RGB565: V4L2 R in the top 5 bits of the 16-bit word */
		HANTRO_PP_REG_WRITE(vpu, r_mask, 0xf800f800);
		HANTRO_PP_REG_WRITE(vpu, g_mask, 0x07e007e0);
		HANTRO_PP_REG_WRITE(vpu, b_mask, 0x001f001f);
		HANTRO_PP_REG_WRITE(vpu, rgb_r_padd, 0);
		HANTRO_PP_REG_WRITE(vpu, rgb_g_padd, 5);
		HANTRO_PP_REG_WRITE(vpu, rgb_b_padd, 11);
	} else {		/* XRGB32: word 0xffRRGGBB */
		HANTRO_PP_REG_WRITE(vpu, r_mask, 0xff000000 | 0x00ff0000);
		HANTRO_PP_REG_WRITE(vpu, g_mask, 0xff000000 | 0x0000ff00);
		HANTRO_PP_REG_WRITE(vpu, b_mask, 0xff000000 | 0x000000ff);
		HANTRO_PP_REG_WRITE(vpu, rgb_r_padd, 8);
		HANTRO_PP_REG_WRITE(vpu, rgb_g_padd, 16);
		HANTRO_PP_REG_WRITE(vpu, rgb_b_padd, 24);
	}
}

static void hantro_postproc_g1_enable(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *dst_buf;
	u32 src_pp_fmt, dst_pp_fmt;
	dma_addr_t dst_dma;

	/* Turn on pipeline mode. Must be done first. */
	HANTRO_PP_REG_WRITE(vpu, pipeline_en, 0x1);

	src_pp_fmt = VPU_PP_IN_NV12;

	switch (ctx->vpu_dst_fmt->fourcc) {
	case V4L2_PIX_FMT_NV12:
		dst_pp_fmt = VPU_PP_OUT_NV12;
		break;
	case V4L2_PIX_FMT_YUYV:
		dst_pp_fmt = VPU_PP_OUT_YUYV;
		break;
	case V4L2_PIX_FMT_XRGB32:
	case V4L2_PIX_FMT_RGB565:
		dst_pp_fmt = VPU_PP_OUT_RGB;
		break;
	default:
		WARN(1, "output format %d not supported by the post-processor, this wasn't expected.",
		     ctx->vpu_dst_fmt->fourcc);
		dst_pp_fmt = 0;
		break;
	}

	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	dst_dma = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);

	HANTRO_PP_REG_WRITE(vpu, clk_gate, 0x1);
	if (hantro_postproc_is_rgb(ctx->vpu_dst_fmt->fourcc)) {
		/* Vendor ppcfg.h: RGB32/RGB16 outputs are big endian with 32-bit word swap */
		bool rgb16 = ctx->vpu_dst_fmt->fourcc == V4L2_PIX_FMT_RGB565;

		HANTRO_PP_REG_WRITE(vpu, out_endian, 0x0);
		HANTRO_PP_REG_WRITE(vpu, out_swap32, 0x1);
		HANTRO_PP_REG_WRITE(vpu, out_swap16, rgb16);
		hantro_postproc_g1_rgb(ctx, rgb16);
	} else {
		HANTRO_PP_REG_WRITE(vpu, out_endian, 0x1);
		HANTRO_PP_REG_WRITE(vpu, out_swap32, 0x1);
		HANTRO_PP_REG_WRITE(vpu, out_swap16, 0x0);
	}
	HANTRO_PP_REG_WRITE(vpu, max_burst, 16);
	HANTRO_PP_REG_WRITE(vpu, out_luma_base, dst_dma);
	if (ctx->vpu_dst_fmt->fourcc == V4L2_PIX_FMT_NV12)
		HANTRO_PP_REG_WRITE(vpu, out_chroma_base,
				    dst_dma + ctx->dst_fmt.plane_fmt[0].bytesperline *
				    ctx->dst_fmt.height);
	/* The PP input is the decoded (reference) picture, not the output. */
	HANTRO_PP_REG_WRITE(vpu, input_fmt, src_pp_fmt);
	HANTRO_PP_REG_WRITE(vpu, output_fmt, dst_pp_fmt);
	HANTRO_PP_REG_WRITE(vpu, output_width, ctx->dst_fmt.width);
	HANTRO_PP_REG_WRITE(vpu, output_height, ctx->dst_fmt.height);
	HANTRO_PP_REG_WRITE(vpu, output_width_ext, ctx->dst_fmt.width >> 11);
	HANTRO_PP_REG_WRITE(vpu, output_height_ext, ctx->dst_fmt.height >> 11);
	hantro_postproc_g1_scale(ctx);
	hantro_postproc_g1_crop(ctx);
	HANTRO_PP_REG_WRITE(vpu, orig_width, MB_WIDTH(ctx->ref_fmt.width));
	HANTRO_PP_REG_WRITE(vpu, display_width, ctx->dst_fmt.width);
	/*
	 * Height extension is MBs >> 8 (8-bit field in swreg72). The 6.18
	 * code has MB_HEIGHT(h >> 8), which yields 1 for any normal height.
	 * Input size and crop are written by hantro_postproc_g1_crop().
	 */
}

static int down_scale_factor(struct hantro_ctx *ctx)
{
	if (ctx->src_fmt.width <= ctx->dst_fmt.width)
		return 0;

	return DIV_ROUND_CLOSEST(ctx->src_fmt.width, ctx->dst_fmt.width);
}

static void hantro_postproc_g2_enable(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *dst_buf;
	int down_scale = down_scale_factor(ctx);
	int out_depth;
	size_t chroma_offset;
	dma_addr_t dst_dma;

	dst_buf = hantro_get_dst_buf(ctx);
	dst_dma = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	chroma_offset = ctx->dst_fmt.plane_fmt[0].bytesperline *
			ctx->dst_fmt.height;

	if (down_scale) {
		hantro_reg_write(vpu, &g2_down_scale_e, 1);
		hantro_reg_write(vpu, &g2_down_scale_y, down_scale >> 2);
		hantro_reg_write(vpu, &g2_down_scale_x, down_scale >> 2);
		hantro_write_addr(vpu, G2_DS_DST, dst_dma);
		hantro_write_addr(vpu, G2_DS_DST_CHR, dst_dma + (chroma_offset >> down_scale));
	} else {
		hantro_write_addr(vpu, G2_RS_OUT_LUMA_ADDR, dst_dma);
		hantro_write_addr(vpu, G2_RS_OUT_CHROMA_ADDR, dst_dma + chroma_offset);
	}

	out_depth = hantro_get_format_depth(ctx->dst_fmt.pixelformat);
	if (ctx->dev->variant->legacy_regs) {
		u8 pp_shift = 0;

		if (out_depth > 8)
			pp_shift = 16 - out_depth;

		hantro_reg_write(ctx->dev, &g2_rs_out_bit_depth, out_depth);
		hantro_reg_write(ctx->dev, &g2_pp_pix_shift, pp_shift);
	} else {
		hantro_reg_write(vpu, &g2_output_8_bits, out_depth > 8 ? 0 : 1);
		hantro_reg_write(vpu, &g2_output_format, out_depth > 8 ? 1 : 0);
	}
	hantro_reg_write(vpu, &g2_out_rs_e, 1);
}

static int hantro_postproc_g2_enum_framesizes(struct hantro_ctx *ctx,
					      struct v4l2_frmsizeenum *fsize)
{
	/**
	 * G2 scaler can scale down by 0, 2, 4 or 8
	 * use fsize->index has power of 2 diviser
	 **/
	if (fsize->index > 3)
		return -EINVAL;

	if (!ctx->src_fmt.width || !ctx->src_fmt.height)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = ctx->src_fmt.width >> fsize->index;
	fsize->discrete.height = ctx->src_fmt.height >> fsize->index;

	return 0;
}

void hantro_postproc_free(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	unsigned int i;

	for (i = 0; i < VB2_MAX_FRAME; ++i) {
		struct hantro_aux_buf *priv = &ctx->postproc.dec_q[i];

		if (priv->cpu) {
			dma_free_attrs(vpu->dev, priv->size, priv->cpu,
				       priv->dma, priv->attrs);
			priv->cpu = NULL;
		}
	}
}

static unsigned int hantro_postproc_buffer_size(struct hantro_ctx *ctx)
{
	unsigned int buf_size;

	buf_size = ctx->ref_fmt.plane_fmt[0].sizeimage;
	if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_H264_SLICE)
		buf_size += hantro_h264_mv_size(ctx->ref_fmt.width,
						ctx->ref_fmt.height);
	else if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_VP9_FRAME)
		buf_size += hantro_vp9_mv_size(ctx->ref_fmt.width,
					       ctx->ref_fmt.height);
	else if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_HEVC_SLICE)
		buf_size += hantro_hevc_mv_size(ctx->ref_fmt.width,
						ctx->ref_fmt.height);
	else if (ctx->vpu_src_fmt->fourcc == V4L2_PIX_FMT_AV1_FRAME)
		buf_size += hantro_av1_mv_size(ctx->ref_fmt.width,
					       ctx->ref_fmt.height);

	return buf_size;
}

static int hantro_postproc_alloc(struct hantro_ctx *ctx, int index)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_aux_buf *priv = &ctx->postproc.dec_q[index];
	unsigned int buf_size = hantro_postproc_buffer_size(ctx);

	if (!buf_size)
		return -EINVAL;

	/*
	 * The buffers on this queue are meant as intermediate
	 * buffers for the decoder, so no mapping is needed.
	 */
	priv->attrs = DMA_ATTR_NO_KERNEL_MAPPING;
	priv->cpu = dma_alloc_attrs(vpu->dev, buf_size, &priv->dma,
				    GFP_KERNEL, priv->attrs);
	if (!priv->cpu)
		return -ENOMEM;
	priv->size = buf_size;

	return 0;
}

int hantro_postproc_init(struct hantro_ctx *ctx)
{
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct vb2_queue *cap_queue = &m2m_ctx->cap_q_ctx.q;
	unsigned int num_buffers = cap_queue->num_buffers;
	unsigned int i;
	int ret;

	for (i = 0; i < num_buffers; i++) {
		ret = hantro_postproc_alloc(ctx, i);
		if (ret) {
			hantro_postproc_free(ctx);
			return ret;
		}
	}

	return 0;
}

dma_addr_t
hantro_postproc_get_dec_buf_addr(struct hantro_ctx *ctx, int index)
{
	struct hantro_aux_buf *priv = &ctx->postproc.dec_q[index];
	unsigned int buf_size = hantro_postproc_buffer_size(ctx);
	struct hantro_dev *vpu = ctx->dev;
	int ret;

	if (priv->size < buf_size && priv->cpu) {
		/* buffer is too small, release it */
		dma_free_attrs(vpu->dev, priv->size, priv->cpu,
			       priv->dma, priv->attrs);
		priv->cpu = NULL;
	}

	if (!priv->cpu) {
		/* buffer not already allocated, try getting a new one */
		ret = hantro_postproc_alloc(ctx, index);
		if (ret)
			return 0;
	}

	if (!priv->cpu)
		return 0;

	return priv->dma;
}

static void hantro_postproc_g1_disable(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	HANTRO_PP_REG_WRITE(vpu, pipeline_en, 0x0);
}

static void hantro_postproc_g2_disable(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	hantro_reg_write(vpu, &g2_out_rs_e, 0);
}

void hantro_postproc_disable(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	if (vpu->variant->postproc_ops && vpu->variant->postproc_ops->disable)
		vpu->variant->postproc_ops->disable(ctx);
}

void hantro_postproc_enable(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	if (vpu->variant->postproc_ops && vpu->variant->postproc_ops->enable)
		vpu->variant->postproc_ops->enable(ctx);
}

int hanto_postproc_enum_framesizes(struct hantro_ctx *ctx,
				   struct v4l2_frmsizeenum *fsize)
{
	struct hantro_dev *vpu = ctx->dev;

	if (vpu->variant->postproc_ops && vpu->variant->postproc_ops->enum_framesizes)
		return vpu->variant->postproc_ops->enum_framesizes(ctx, fsize);

	return -EINVAL;
}

const struct hantro_postproc_ops hantro_g1_postproc_ops = {
	.enable = hantro_postproc_g1_enable,
	.disable = hantro_postproc_g1_disable,
	.enum_framesizes = hantro_postproc_g1_enum_framesizes,
};

const struct hantro_postproc_ops hantro_g2_postproc_ops = {
	.enable = hantro_postproc_g2_enable,
	.disable = hantro_postproc_g2_disable,
	.enum_framesizes = hantro_postproc_g2_enum_framesizes,
};
