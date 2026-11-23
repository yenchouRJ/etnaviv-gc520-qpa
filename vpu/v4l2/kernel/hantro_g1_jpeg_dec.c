// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver - G1 family JPEG decoder (Nuvoton MA35D1 VC8000NanoD)
 *
 * Baseline, sequential, interleaved JPEG (8 bit, Huffman) is decoded by the HW and goes
 * straight into the post-processor in combined (pipeline) mode, so no full-size decoded
 * picture is written to memory.  Register layout: Decoder_swreg_map_v6 (swreg3-8, 12, 15-27,
 * 40); the Huffman code counts live in registers, the Huffman values and the quantisation
 * tables in a table buffer (swreg40).  Programming order follows the vendor JpegDecInitHW().
 *
 * Not supported (rejected with -EINVAL, the job fails, no HW access): progressive, lossless,
 * arithmetic, 12 bit, non-interleaved scans, CMYK, sizes outside 33..4096.
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <asm/unaligned.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_g1_regs.h"
#include "hantro_jpeg.h"

/* Decoder_swreg_map_v6 */
#define JPG_CTRL0_DEC_MODE_JPEG		(3U << 28)
#define JPG_CTRL0_DEC_OUT_DIS		BIT(15)
#define JPG_CTRL0_FILTERING_DIS		BIT(14)
#define JPG_CTRL1_MB_WIDTH(v)		(((v) & 0x1ff) << 23)
#define JPG_CTRL1_MB_HEIGHT(v)		(((v) & 0xff) << 11)
#define JPG_CTRL1_MB_W_EXT(v)		((((v) >> 9) & 0x7) << 3)
#define JPG_CTRL1_MB_H_EXT(v)		(((v) >> 8) & 0x7)
#define JPG_CTRL2_STRM_START_BIT(v)	(((v) & 0x3f) << 26)
#define JPG_CTRL2_SYNC_MARKER_E		BIT(25)
#define JPG_CTRL2_QTABLES(v)		(((v) & 0x3) << 11)
#define JPG_CTRL2_MODE(v)		(((v) & 0x7) << 8)
#define JPG_CTRL2_FILRIGHT_E		BIT(7)
#define JPG_CTRL2_STREAM_ALL		BIT(6)
#define JPG_CTRL2_CR_AC_TAB(v)		((v) ? BIT(5) : 0)
#define JPG_CTRL2_CB_AC_TAB(v)		((v) ? BIT(4) : 0)
#define JPG_CTRL2_CR_DC_TAB(v)		((v) ? BIT(3) : 0)
#define JPG_CTRL2_CB_DC_TAB(v)		((v) ? BIT(2) : 0)
#define JPG_CTRL3_STREAM_LEN(v)		((v) & 0xffffff)
#define JPG_CTRL4_FILDOWN_E		BIT(26)
#define JPG_CTRL4_SE(v)			((v) & 0xff)
#define JPG_CTRL5_REST_FREQ(v)		((v) & 0xffff)
#define JPG_REG_STR_BASE		G1_REG_ADDR_STR	/* swreg12 */
#define JPG_REG_OUT_LUMA		G1_REG_ADDR_DST	/* swreg13 */
#define JPG_REG_OUT_CHROMA		G1_SWREG(14)
#define JPG_REG_SLICE_H			G1_SWREG(15)
#define JPG_REG_AC1_BASE		G1_SWREG(16)	/* swreg16-22: AC1/AC2 code counts */
#define JPG_REG_DC1_BASE		G1_SWREG(23)	/* swreg23-26: DC1/DC2 code counts */

/* HWIF_JPEG_MODE */
#define JPG_MODE_400	0
#define JPG_MODE_420	2
#define JPG_MODE_422	3
#define JPG_MODE_444	4
#define JPG_MODE_440	5
#define JPG_MODE_411	6

#define JPG_MIN_HW_DIM	48
#define JPG_MAX_DIM	4096

/* AC1/AC2 swreg16..22 code-count fields: {swreg offset from 16, width, lsb} for length 1..16 */
static const u8 ac1_fld[16][3] = {
	{0, 2, 0}, {0, 3, 3}, {0, 4, 7}, {0, 5, 11}, {0, 6, 16}, {0, 7, 24},
	{1, 8, 0}, {1, 8, 8}, {1, 8, 16}, {1, 8, 24},
	{2, 8, 0}, {2, 8, 8}, {2, 8, 16}, {2, 8, 24},
	{3, 8, 0}, {3, 8, 8},
};

static const u8 ac2_fld[16][3] = {
	{3, 2, 16}, {3, 3, 19}, {3, 4, 23}, {3, 5, 27},
	{4, 6, 0}, {4, 7, 8}, {4, 8, 16}, {4, 8, 24},
	{5, 8, 0}, {5, 8, 8}, {5, 8, 16}, {5, 8, 24},
	{6, 8, 0}, {6, 8, 8}, {6, 8, 16}, {6, 8, 24},
};

static const u8 zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

struct jpeg_huff {
	const u8 *bits;		/* BITS[16] followed by HUFFVAL */
	unsigned int nvals;
};

struct hantro_jpeg_hdr {
	unsigned int width, height;
	unsigned int nf;
	u8 id[3], h[3], v[3], tq[3], td[3], ta[3];
	u16 restart;
	unsigned int ecs_offset;
	unsigned int mode;
	const u8 *qt[4];		/* 64 bytes, zigzag order */
	struct jpeg_huff dc[2], ac[2];
};

#define MAX_DC_VALS	12
#define MAX_AC_VALS	162

static int jpeg_parse_dqt(const u8 *p, const u8 *end, struct hantro_jpeg_hdr *h)
{
	while (p < end) {
		u8 pq_tq = *p++;

		if ((pq_tq >> 4) != 0 || (pq_tq & 0xf) > 3 || end - p < 64)
			return -EINVAL;
		h->qt[pq_tq & 0xf] = p;
		p += 64;
	}
	return 0;
}

static int jpeg_parse_dht(const u8 *p, const u8 *end, struct hantro_jpeg_hdr *h)
{
	while (p < end) {
		u8 tc_th = *p++;
		unsigned int i, n = 0;
		struct jpeg_huff *t;

		if ((tc_th >> 4) > 1 || (tc_th & 0xf) > 1 || end - p < 16)
			return -EINVAL;
		for (i = 0; i < 16; i++)
			n += p[i];
		if (n > ((tc_th >> 4) ? MAX_AC_VALS : MAX_DC_VALS) || end - p < 16 + n)
			return -EINVAL;
		t = (tc_th >> 4) ? &h->ac[tc_th & 1] : &h->dc[tc_th & 1];
		t->bits = p;
		t->nvals = n;
		p += 16 + n;
	}
	return 0;
}

/* Header of a baseline interleaved JPEG up to and including the SOS header; -EINVAL if unsupported */
static int hantro_jpeg_parse(const u8 *buf, size_t len, struct hantro_jpeg_hdr *h)
{
	const u8 *p = buf, *end = buf + len;
	bool sof = false;
	unsigned int i;

	memset(h, 0, sizeof(*h));
	if (len < 4 || get_unaligned_be16(p) != 0xffd8)
		return -EINVAL;
	p += 2;

	while (end - p >= 4) {
		unsigned int seg;
		const u8 *s;
		u8 m;

		if (*p++ != 0xff)
			return -EINVAL;
		while (p < end && *p == 0xff)
			p++;
		if (p >= end)
			return -EINVAL;
		m = *p++;
		if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7))
			continue;
		if (m == 0xd9 || m == 0x00)
			return -EINVAL;
		if (end - p < 2)
			return -EINVAL;
		seg = get_unaligned_be16(p);
		if (seg < 2 || seg > end - p)
			return -EINVAL;
		s = p + 2;
		switch (m) {
		case 0xc0:		/* SOF0: baseline */
			if (sof || seg < 8 + 3 || s[0] != 8)
				return -EINVAL;
			h->height = get_unaligned_be16(s + 1);
			h->width = get_unaligned_be16(s + 3);
			h->nf = s[5];
			if ((h->nf != 1 && h->nf != 3) || seg != 8 + 3 * h->nf)
				return -EINVAL;
			for (i = 0; i < h->nf; i++) {
				h->id[i] = s[6 + 3 * i];
				h->h[i] = s[7 + 3 * i] >> 4;
				h->v[i] = s[7 + 3 * i] & 0xf;
				h->tq[i] = s[8 + 3 * i];
				if (!h->h[i] || h->h[i] > 4 || !h->v[i] || h->v[i] > 4 || h->tq[i] > 3)
					return -EINVAL;
			}
			sof = true;
			break;
		case 0xc1 ... 0xc3: case 0xc5 ... 0xc7: case 0xc9 ... 0xcb: case 0xcd ... 0xcf:
		case 0xcc:		/* progressive, lossless, differential, arithmetic */
			return -EINVAL;
		case 0xc4:
			if (jpeg_parse_dht(s, p + seg, h))
				return -EINVAL;
			break;
		case 0xdb:
			if (jpeg_parse_dqt(s, p + seg, h))
				return -EINVAL;
			break;
		case 0xdd:
			if (seg != 4)
				return -EINVAL;
			h->restart = get_unaligned_be16(s);
			break;
		case 0xda: {		/* SOS: interleaved, all components, baseline */
			unsigned int ns;

			if (!sof || seg < 6 + 2 * h->nf)
				return -EINVAL;
			ns = s[0];
			if (ns != h->nf || seg != 6 + 2 * ns || s[1 + 2 * ns] != 0 ||
			    s[2 + 2 * ns] != 63 || s[3 + 2 * ns] != 0)
				return -EINVAL;
			for (i = 0; i < ns; i++) {
				/* same components, same order as the frame header */
				if (s[1 + 2 * i] != h->id[i] || (s[2 + 2 * i] >> 4) > 1 ||
				    (s[2 + 2 * i] & 0xf) > 1)
					return -EINVAL;
				h->td[i] = s[2 + 2 * i] >> 4;
				h->ta[i] = s[2 + 2 * i] & 0xf;
			}
			h->ecs_offset = p + seg - buf;
			return 0;
		}
		default:
			break;		/* APPn, COM, ...: skipped */
		}
		p += seg;
	}
	return -EINVAL;
}

/* Fill the HW configuration from the parsed header; -EINVAL for combinations the HW lacks */
static int hantro_jpeg_check(struct hantro_jpeg_hdr *h)
{
	unsigned int i;

	if (ALIGN(h->width, MB_DIM) < JPG_MIN_HW_DIM || ALIGN(h->height, MB_DIM) < JPG_MIN_HW_DIM ||
	    h->width > JPG_MAX_DIM || h->height > JPG_MAX_DIM)
		return -EINVAL;

	if (h->nf == 1) {
		h->mode = JPG_MODE_400;
	} else if (h->h[1] != 1 || h->v[1] != 1 || h->h[2] != 1 || h->v[2] != 1) {
		return -EINVAL;
	} else if (h->h[0] == 2 && h->v[0] == 2) {
		h->mode = JPG_MODE_420;
	} else if (h->h[0] == 2 && h->v[0] == 1) {
		h->mode = JPG_MODE_422;
	} else if (h->h[0] == 1 && h->v[0] == 2) {
		h->mode = JPG_MODE_440;
	} else if (h->h[0] == 1 && h->v[0] == 1) {
		h->mode = JPG_MODE_444;
	} else if (h->h[0] == 4 && h->v[0] == 1) {
		/* 32x8 MCUs: the HW wants a width multiple of 32 (vendor adds 16 otherwise) */
		if (ALIGN(h->width, MB_DIM) & 0x1f)
			return -EINVAL;
		h->mode = JPG_MODE_411;
	} else {
		return -EINVAL;
	}

	for (i = 0; i < h->nf; i++)
		if (!h->qt[h->tq[i]])
			return -EINVAL;
	return 0;
}

/* Huffman code counts of a table into registers: {reg offset, width, lsb} per length */
static void jpeg_set_ac_counts(const struct jpeg_huff *t, const u8 fld[16][3], u32 regs[7])
{
	unsigned int i;

	for (i = 0; i < 16; i++)
		regs[fld[i][0]] |= ((u32)t->bits[i] & ((1U << fld[i][1]) - 1)) << fld[i][2];
}

static void jpeg_set_dc_counts(const struct jpeg_huff *t, u32 regs[2])
{
	unsigned int i;
	static const u8 lsb_lo[8] = { 0, 4, 8, 12, 16, 20, 24, 28 };
	static const u8 w_lo[8] = { 2, 3, 4, 4, 4, 4, 4, 4 };

	/*
	 * swreg23/25: lengths 1..8 (2,3,4,4,... bits wide, nibble spaced from bit 0, 4, 8...),
	 * swreg24/26: lengths 9..16 (4 bits each, bit 0, 4, 8, ...).
	 */
	regs[0] = regs[1] = 0;
	for (i = 0; i < 8; i++)
		regs[0] |= ((u32)t->bits[i] & ((1U << w_lo[i]) - 1)) << lsb_lo[i];
	for (i = 0; i < 8; i++)
		regs[1] |= ((u32)t->bits[8 + i] & 0xf) << (4 * i);
}

static const struct jpeg_huff *jpeg_pick(const struct jpeg_huff *t, struct jpeg_huff *std,
					 bool ac, bool chroma)
{
	size_t len;

	if (t->bits)
		return t;
	std->bits = hantro_jpeg_std_huffman(ac, chroma, &len);
	std->nvals = len - 16;
	return std;
}

/* Table buffer: quantisation tables (raster order), AC1, AC2, DC1, DC2 values as a byte stream */
static size_t jpeg_build_tables(struct hantro_jpeg_hdr *h, const struct jpeg_huff *ac[2],
				const struct jpeg_huff *dc[2], u8 *out)
{
	unsigned int i, j;
	u8 *p = out;

	for (j = 0; j < h->nf; j++) {
		const u8 *q = h->qt[h->tq[j]];

		for (i = 0; i < 64; i++)
			p[zigzag[i]] = q[i];
		p += 64;
	}
	for (j = 0; j < 2; j++) {
		memset(p, 0, MAX_AC_VALS);
		if (ac[j])
			memcpy(p, ac[j]->bits + 16, ac[j]->nvals);
		p += MAX_AC_VALS;
	}
	for (j = 0; j < 2; j++) {
		memset(p, 0, MAX_DC_VALS);
		if (dc[j])
			memcpy(p, dc[j]->bits + 16, dc[j]->nvals);
		p += MAX_DC_VALS;
	}
	return p - out;
}

int hantro_jpeg_dec_init(struct hantro_ctx *ctx)
{
	struct hantro_aux_buf *t = &ctx->jpeg_dec.tables;

	/* 3 x 64 quantisation + 2 x 162 AC + 2 x 12 DC bytes, padded to words */
	t->size = ALIGN(3 * 64 + 2 * MAX_AC_VALS + 2 * MAX_DC_VALS, 4);
	t->cpu = dma_alloc_coherent(ctx->dev->dev, t->size, &t->dma, GFP_KERNEL);
	if (!t->cpu)
		return -ENOMEM;
	return 0;
}

void hantro_jpeg_dec_exit(struct hantro_ctx *ctx)
{
	struct hantro_aux_buf *t = &ctx->jpeg_dec.tables;

	if (t->cpu)
		dma_free_coherent(ctx->dev->dev, t->size, t->cpu, t->dma);
	t->cpu = NULL;
}

/* PP input format for the decoder's JPEG sampling (vendor ppapi.c, pipelined JPEG) */
static void jpeg_pp_input(struct hantro_ctx *ctx, unsigned int mode)
{
	switch (mode) {
	case JPG_MODE_400:
		ctx->jpeg_dec.pp_in = 3;
		ctx->jpeg_dec.pp_in_es = 0;
		break;
	case JPG_MODE_420:
		ctx->jpeg_dec.pp_in = 1;
		ctx->jpeg_dec.pp_in_es = 0;
		break;
	case JPG_MODE_422:
		ctx->jpeg_dec.pp_in = 4;
		ctx->jpeg_dec.pp_in_es = 0;
		break;
	case JPG_MODE_440:
		ctx->jpeg_dec.pp_in = 6;
		ctx->jpeg_dec.pp_in_es = 0;
		break;
	case JPG_MODE_444:
		ctx->jpeg_dec.pp_in = 7;
		ctx->jpeg_dec.pp_in_es = 0;
		break;
	default:		/* 4:1:1 */
		ctx->jpeg_dec.pp_in = 7;
		ctx->jpeg_dec.pp_in_es = 1;
		break;
	}
}

static void jpeg_write_tables(struct hantro_ctx *ctx, struct hantro_jpeg_hdr *h,
			      const struct jpeg_huff *ac[2], const struct jpeg_huff *dc[2])
{
	struct hantro_aux_buf *t = &ctx->jpeg_dec.tables;
	u8 bytes[3 * 64 + 2 * MAX_AC_VALS + 2 * MAX_DC_VALS + 4] = {};
	u32 *w = t->cpu;
	size_t n, i;

	n = jpeg_build_tables(h, ac, dc, bytes);
	/* HW reads the table words big endian: first byte in bits 31:24 */
	for (i = 0; i < ALIGN(n, 4) / 4; i++)
		w[i] = (u32)bytes[4 * i] << 24 | bytes[4 * i + 1] << 16 |
		       bytes[4 * i + 2] << 8 | bytes[4 * i + 3];
}

int hantro_g1_jpeg_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf = hantro_get_src_buf(ctx);
	struct hantro_jpeg_hdr hdr;
	struct jpeg_huff std_ac[2], std_dc[2];
	const struct jpeg_huff *ac[2], *dc[2];
	u32 ac1[7] = {}, dc1[2], dc2[2];
	unsigned int mbw, mbh, lt, i;
	size_t payload = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	dma_addr_t src_dma;
	u32 ctrl2, stream_off;
	void *vaddr;
	int ret;

	vaddr = vb2_plane_vaddr(&src_buf->vb2_buf, 0);
	if (!vaddr) {
		vpu_err("JPEG: no kernel mapping of the OUTPUT buffer\n");
		return -EINVAL;
	}

	ret = hantro_jpeg_parse(vaddr, payload, &hdr);
	if (!ret)
		ret = hantro_jpeg_check(&hdr);
	if (ret) {
		vpu_err("JPEG: stream not supported (baseline interleaved 8 bit, 33..%u)\n",
			JPG_MAX_DIM);
		return ret;
	}
	if (ALIGN(hdr.width, MB_DIM) != ctx->src_fmt.width ||
	    ALIGN(hdr.height, MB_DIM) != ctx->src_fmt.height) {
		vpu_err("JPEG: %ux%u does not match the OUTPUT format %ux%u\n",
			hdr.width, hdr.height, ctx->src_fmt.width, ctx->src_fmt.height);
		return -EINVAL;
	}

	/* Huffman tables: a missing DHT means the standard tables (T.81 Annex K) */
	lt = hdr.ta[0];
	ac[0] = jpeg_pick(&hdr.ac[lt], &std_ac[0], true, lt);
	ac[1] = hdr.ac[!lt].bits ? &hdr.ac[!lt] : (hdr.nf > 1 ? jpeg_pick(&hdr.ac[!lt], &std_ac[1], true, !lt) : NULL);
	dc[0] = jpeg_pick(&hdr.dc[hdr.td[0]], &std_dc[0], false, hdr.td[0]);
	dc[1] = hdr.dc[!hdr.td[0]].bits ? &hdr.dc[!hdr.td[0]] :
		(hdr.nf > 1 ? jpeg_pick(&hdr.dc[!hdr.td[0]], &std_dc[1], false, !hdr.td[0]) : NULL);

	jpeg_pp_input(ctx, hdr.mode);
	/* Picture size in MBs (vendor: 4:2:2 / 4:1:1 heights and 4:4:0 / 4:4:4 / 4:0:0 widths fill) */
	mbw = ALIGN(hdr.width, MB_DIM) / MB_DIM;
	mbh = ALIGN(hdr.height, MB_DIM) / MB_DIM;

	hantro_start_prepare_run(ctx);

	jpeg_write_tables(ctx, &hdr, ac, dc);
	if (ac[0])
		jpeg_set_ac_counts(ac[0], ac1_fld, ac1);
	if (ac[1])
		jpeg_set_ac_counts(ac[1], ac2_fld, ac1);
	if (dc[0])
		jpeg_set_dc_counts(dc[0], dc1);
	else
		dc1[0] = dc1[1] = 0;
	if (dc[1])
		jpeg_set_dc_counts(dc[1], dc2);
	else
		dc2[0] = dc2[1] = 0;

	/* Registers the H.264 path leaves non-zero and the JPEG path does not set */
	vdpu_write_relaxed(vpu, 0, G1_SWREG(9));
	vdpu_write_relaxed(vpu, 0, G1_SWREG(10));
	vdpu_write_relaxed(vpu, 0, G1_SWREG(11));
	for (i = 28; i <= 39; i++)
		vdpu_write_relaxed(vpu, 0, G1_SWREG(i));
	for (i = 41; i <= 43; i++)
		vdpu_write_relaxed(vpu, 0, G1_SWREG(i));
	vdpu_write_relaxed(vpu, 0, G1_REG_ERR_CONC);
	vdpu_write_relaxed(vpu, 0, G1_REG_PRED_FLT);
	vdpu_write_relaxed(vpu, 0, G1_REG_REF_BUF_CTRL);
	vdpu_write_relaxed(vpu, 0, G1_REG_REF_BUF_CTRL2);

	/* swreg3: JPEG mode, decoder output disabled (the PP takes the picture), no filter */
	vdpu_write_relaxed(vpu, JPG_CTRL0_DEC_MODE_JPEG | JPG_CTRL0_DEC_OUT_DIS | JPG_CTRL0_FILTERING_DIS,
			   G1_REG_DEC_CTRL0);
	/* swreg4: picture size in macroblocks */
	vdpu_write_relaxed(vpu, JPG_CTRL1_MB_WIDTH(mbw) | JPG_CTRL1_MB_HEIGHT(mbh) |
			   JPG_CTRL1_MB_W_EXT(mbw) | JPG_CTRL1_MB_H_EXT(mbh), G1_REG_DEC_CTRL1);

	/* swreg5: stream start bit, restart, table counts, sampling, fill, chroma table selectors */
	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	stream_off = hdr.ecs_offset;
	ctrl2 = JPG_CTRL2_STRM_START_BIT((stream_off & 7) * 8) |
		JPG_CTRL2_QTABLES(hdr.nf == 1 ? 1 : 3) |
		JPG_CTRL2_MODE(hdr.mode) | JPG_CTRL2_STREAM_ALL;
	if (hdr.restart)
		ctrl2 |= JPG_CTRL2_SYNC_MARKER_E;
	/* 8-pixel remainder: HW fills one block of zeros (width: 4:0:0/4:4:0/4:4:4, height: 4:2:2/4:1:1/4:4:4/4:0:0) */
	{
		bool fr = (hdr.width & 0xf) && (hdr.width & 0xf) <= 8;
		bool fd = (hdr.height & 0xf) && (hdr.height & 0xf) <= 8;
		bool rfill = hdr.mode == JPG_MODE_400 || hdr.mode == JPG_MODE_440 || hdr.mode == JPG_MODE_444;
		bool dfill = hdr.mode == JPG_MODE_400 || hdr.mode == JPG_MODE_422 ||
			     hdr.mode == JPG_MODE_444 || hdr.mode == JPG_MODE_411;

		if (fr && rfill)
			ctrl2 |= JPG_CTRL2_FILRIGHT_E;
		if (fd && dfill)
			vdpu_write_relaxed(vpu, JPG_CTRL4_FILDOWN_E | JPG_CTRL4_SE(63), G1_REG_DEC_CTRL4);
		else
			vdpu_write_relaxed(vpu, JPG_CTRL4_SE(63), G1_REG_DEC_CTRL4);
	}
	/* HW luma table is always "table 1"; chroma picks table 1 or 2 by whether it equals the luma one */
	if (hdr.nf > 1) {
		ctrl2 |= JPG_CTRL2_CB_AC_TAB(hdr.ta[1] != hdr.ta[0]) | JPG_CTRL2_CR_AC_TAB(hdr.ta[2] != hdr.ta[0]) |
			 JPG_CTRL2_CB_DC_TAB(hdr.td[1] != hdr.td[0]) | JPG_CTRL2_CR_DC_TAB(hdr.td[2] != hdr.td[0]);
	}
	vdpu_write_relaxed(vpu, ctrl2, G1_REG_DEC_CTRL2);

	/* swreg6: stream length from the 8-byte aligned start */
	vdpu_write_relaxed(vpu, JPG_CTRL3_STREAM_LEN(payload - (stream_off & ~7U)), G1_REG_DEC_CTRL3);
	/* swreg8 (offset 0x20): restart interval */
	vdpu_write_relaxed(vpu, JPG_CTRL5_REST_FREQ(hdr.restart), G1_REG_DEC_CTRL5);
	/* swreg12: stream base (ECS start rounded down to 8 bytes) */
	vdpu_write_relaxed(vpu, (src_dma + stream_off) & ~7ULL, JPG_REG_STR_BASE);
	/* swreg13/14: decoder output not written (pipeline) */
	vdpu_write_relaxed(vpu, 0, JPG_REG_OUT_LUMA);
	vdpu_write_relaxed(vpu, 0, JPG_REG_OUT_CHROMA);
	vdpu_write_relaxed(vpu, 0, JPG_REG_SLICE_H);	/* whole picture, no slices */

	/* swreg16-22: AC code counts (the second table's fields sit in the same words) */
	for (i = 0; i < 7; i++)
		vdpu_write_relaxed(vpu, ac1[i], JPG_REG_AC1_BASE + 4 * i);
	vdpu_write_relaxed(vpu, dc1[0], G1_SWREG(23));
	vdpu_write_relaxed(vpu, dc1[1], G1_SWREG(24));
	vdpu_write_relaxed(vpu, dc2[0], G1_SWREG(25));
	vdpu_write_relaxed(vpu, dc2[1], G1_SWREG(26));
	vdpu_write_relaxed(vpu, 0, G1_SWREG(27));
	vdpu_write_relaxed(vpu, ctx->jpeg_dec.tables.dma, G1_REG_ADDR_QTABLE);

	hantro_end_prepare_run(ctx);

	vdpu_write_relaxed(vpu,
			   G1_REG_CONFIG_DEC_AXI_RD_ID(0xffu) |
			   G1_REG_CONFIG_DEC_TIMEOUT_E |
			   G1_REG_CONFIG_DEC_OUT_ENDIAN |
			   G1_REG_CONFIG_DEC_STRENDIAN_E |
			   G1_REG_CONFIG_DEC_MAX_BURST(16) |
			   G1_REG_CONFIG_DEC_OUTSWAP32_E |
			   G1_REG_CONFIG_DEC_INSWAP32_E |
			   G1_REG_CONFIG_DEC_STRSWAP32_E |
			   G1_REG_CONFIG_DEC_CLK_GATE_E,
			   G1_REG_CONFIG);
	vdpu_write(vpu, G1_REG_INTERRUPT_DEC_E, G1_REG_INTERRUPT);

	return 0;
}
