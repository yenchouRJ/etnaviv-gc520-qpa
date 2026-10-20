// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU driver - Nuvoton MA35D1 VC8000NanoD variant
 *
 * H.264 stateless decode only, NV12 output straight from the decoder
 * (post-processor not used).  Register layout is G1-compatible, so the
 * mainline G1 H.264 code is reused unchanged.
 */

#include <linux/module.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_g1_regs.h"

/*
 * Soft reset register (swreg101, 0x194) is not documented in the NanoD
 * register map, so it is not written unless asked to.
 */
static bool ma35d1_soft_reset;
module_param_named(ma35d1_soft_reset, ma35d1_soft_reset, bool, 0644);
MODULE_PARM_DESC(ma35d1_soft_reset, "Write swreg101 soft reset on ctx reset (default 0)");

static const struct hantro_fmt ma35d1_vpu_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = HANTRO_MODE_NONE,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_FHD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_FHD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_FHD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_FHD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
};

static void ma35d1_vpu_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	vdpu_write(vpu, G1_REG_INTERRUPT_DEC_IRQ_DIS, G1_REG_INTERRUPT);
	vdpu_write(vpu, G1_REG_CONFIG_DEC_CLK_GATE_E, G1_REG_CONFIG);
	if (ma35d1_soft_reset)
		vdpu_write(vpu, 1, G1_REG_SOFT_RESET);
}

/*
 * Clocks are only guaranteed on while a job runs, so log the ID register
 * from the first run instead of from probe.
 */
static int ma35d1_vpu_h264_run(struct hantro_ctx *ctx)
{
	static bool id_logged;

	if (!id_logged) {
		struct hantro_dev *vpu = ctx->dev;

		u32 pp_fuse, pp_cfg;

		id_logged = true;
		dev_info(vpu->dev, "HW id 0x%08x swreg2 0x%08x\n",
			 vdpu_read(vpu, 0), vdpu_read(vpu, G1_REG_CONFIG));

		/* Synthesis/fuse registers (read only), see pp/plan.md T0. */
		pp_fuse = vdpu_read(vpu, 0x18c);
		pp_cfg = vdpu_read(vpu, 0x190);
		dev_info(vpu->dev,
			 "dec synth swreg50 0x%08x swreg54 0x%08x fuse swreg57 0x%08x\n",
			 vdpu_read(vpu, 0xc8), vdpu_read(vpu, 0xd8),
			 vdpu_read(vpu, 0xe4));
		dev_info(vpu->dev,
			 "pp fuse swreg99 0x%08x synth swreg100 0x%08x: scale_level %u pixac %u tiled %u deint %u blend %u maxw(4k %u 1920 %u 1280 %u)\n",
			 pp_fuse, pp_cfg, (pp_cfg >> 26) & 3,
			 (pp_cfg >> 30) & 1, (pp_cfg >> 29) & 1,
			 (pp_cfg >> 25) & 1, (pp_cfg >> 24) & 1,
			 (pp_fuse >> 16) & 1, (pp_fuse >> 15) & 1,
			 (pp_fuse >> 14) & 1);
	}

	return hantro_g1_h264_dec_run(ctx);
}

static const struct hantro_codec_ops ma35d1_vpu_codec_ops[] = {
	[HANTRO_MODE_H264_DEC] = {
		.run = ma35d1_vpu_h264_run,
		.reset = ma35d1_vpu_reset,
		.init = hantro_h264_dec_init,
		.exit = hantro_h264_dec_exit,
	},
};

static const struct hantro_irq ma35d1_vpu_irqs[] = {
	{ "vpu", hantro_g1_irq },
};

static const char * const ma35d1_vpu_clk_names[] = { "vpu" };

const struct hantro_variant ma35d1_vpu_variant = {
	.dec_fmts = ma35d1_vpu_fmts,
	.num_dec_fmts = ARRAY_SIZE(ma35d1_vpu_fmts),
	.codec = HANTRO_H264_DECODER,
	.codec_ops = ma35d1_vpu_codec_ops,
	.irqs = ma35d1_vpu_irqs,
	.num_irqs = ARRAY_SIZE(ma35d1_vpu_irqs),
	.clk_names = ma35d1_vpu_clk_names,
	.num_clocks = ARRAY_SIZE(ma35d1_vpu_clk_names),
};
