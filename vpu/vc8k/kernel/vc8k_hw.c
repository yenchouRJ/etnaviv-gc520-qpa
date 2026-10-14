// SPDX-License-Identifier: GPL-2.0-only
/*
 * VC8000NanoD register access, job start and interrupt handling.
 *
 * Register sequencing follows the legacy V4L2 glue (vc_hw_engine.c):
 *  - decoder: bit 7 of reg 2 cleared, regs 2..59 then reg 1 (enable) last
 *  - PP: regs 61..100 (reg 91 forced to 0), then reg 60 (enable) last
 *  - interrupt status is acknowledged by writing 0 to reg 1 / reg 60
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>

#include "vc8k_drv.h"

unsigned int vc8k_pp_force_raster = 1;
module_param_named(pp_force_raster, vc8k_pp_force_raster, uint, 0644);
MODULE_PARM_DESC(pp_force_raster,
		 "Force PP reg 91 to 0 like the legacy driver (default 1)");

/* Registers that identify the core and its synthesis configuration. */
static const u8 vc8k_cap_regs[] = { 0, 50, 54, 57, 99, 100 };

void vc8k_hw_read_caps(struct vc8k_dev *v)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc8k_cap_regs); i++)
		v->caps[vc8k_cap_regs[i]] = vc8k_rd(v, vc8k_cap_regs[i]);
}

/* Caller must guarantee no job is being started concurrently. */
void vc8k_hw_reset(struct vc8k_dev *v)
{
	u32 val;
	int i;

	if (vc8k_rd(v, VC8K_REG_DEC_CTRL) & VC8K_STATUS_ENABLE) {
		vc8k_wr(v, VC8K_REG_DEC_CTRL,
			VC8K_STATUS_ABORT | VC8K_STATUS_IRQ_DIS);
		/* Never touch the registers while the core is still busy. */
		if (readl_poll_timeout_atomic(v->mmio + VC8K_REG_DEC_CTRL * 4,
					      val,
					      !(val & VC8K_STATUS_ENABLE),
					      10, 20000))
			dev_err(v->dev, "core did not go idle after abort\n");
	}

	if (v->rstc && vc8k_rd(v, VC8K_REG_DEC_CTRL) & VC8K_STATUS_ENABLE) {
		reset_control_assert(v->rstc);
		udelay(10);
		reset_control_deassert(v->rstc);
	}

	for (i = 1; i < VC8K_NUM_REGS; i++)
		vc8k_wr(v, i, 0);
}

/* Called from run_job() with hw_lock held. */
void vc8k_hw_run(struct vc8k_dev *v, struct vc8k_job *job)
{
	const u32 *r = job->regs;
	int i;

	if (job->flags & VC8K_SUBMIT_PP) {
		for (i = VC8K_PP_FIRST_REG + 1; i <= VC8K_PP_LAST_REG; i++) {
			if (i == VC8K_REG_PP_RASTER && vc8k_pp_force_raster)
				vc8k_wr(v, i, 0);
			else
				vc8k_wr(v, i, r[i]);
		}
		/* A PP-only job starts here; in a combined job it arms. */
		vc8k_wr(v, VC8K_REG_PP_CTRL, r[VC8K_REG_PP_CTRL]);
	}

	if (job->flags & VC8K_SUBMIT_DEC) {
		for (i = 2; i <= VC8K_DEC_LAST_REG; i++) {
			u32 val = r[i];

			if (i == VC8K_REG_DEC_CFG2)
				val &= ~VC8K_DEC_CFG2_BIT7;
			vc8k_wr(v, i, val);
		}
		wmb();
		vc8k_wr(v, VC8K_REG_DEC_CTRL, r[VC8K_REG_DEC_CTRL]);
	}
}

static void vc8k_snapshot(struct vc8k_dev *v, struct vc8k_job *job)
{
	int i;

	for (i = 0; i < VC8K_NUM_REGS; i++)
		job->snap[i] = vc8k_rd(v, i);
	if (job->dec_done)
		job->snap[VC8K_REG_DEC_CTRL] = job->dec_status;
	if (job->pp_done)
		job->snap[VC8K_REG_PP_CTRL] = job->pp_status;
}

irqreturn_t vc8k_irq(int irq, void *data)
{
	struct vc8k_dev *v = data;
	struct vc8k_job *job;
	u32 dec, pp;
	bool handled = false, complete = false;

	spin_lock(&v->hw_lock);

	dec = vc8k_rd(v, VC8K_REG_DEC_CTRL);
	pp = vc8k_rd(v, VC8K_REG_PP_CTRL);
	job = v->cur_job;

	if (dec & VC8K_STATUS_IRQ) {
		handled = true;
		if (job) {
			job->dec_done = true;
			job->dec_status = dec;
			vc8k_snapshot(v, job);
		}
		vc8k_wr(v, VC8K_REG_DEC_CTRL, 0);
	}

	if (pp & VC8K_STATUS_IRQ) {
		handled = true;
		if (job) {
			job->pp_done = true;
			job->pp_status = pp;
			vc8k_snapshot(v, job);
		}
		vc8k_wr(v, VC8K_REG_PP_CTRL, 0);
	}

	if (job) {
		/*
		 * PP-only job: complete on the PP interrupt.  Anything that
		 * involves the decoder (incl. combined pipeline) completes on
		 * the decoder interrupt.
		 */
		if (job->flags & VC8K_SUBMIT_DEC)
			complete = job->dec_done;
		else
			complete = job->pp_done;
	}

	if (complete) {
		v->cur_job = NULL;
		job->result = VC8K_RESULT_OK;
		dma_fence_signal_locked(job->hw_fence);
	}

	spin_unlock(&v->hw_lock);

	if (handled)
		atomic_inc(&v->irq_count);

	return handled ? IRQ_HANDLED : IRQ_NONE;
}
