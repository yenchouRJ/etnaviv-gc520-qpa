// SPDX-License-Identifier: GPL-2.0-only
/* drm_sched glue: one hardware queue, one job on the core at a time. */
#include <linux/interrupt.h>
#include <linux/slab.h>

#include "vc8k_drv.h"

unsigned int vc8k_timeout_ms = 1000;
module_param_named(timeout_ms, vc8k_timeout_ms, uint, 0644);
MODULE_PARM_DESC(timeout_ms, "Hardware job watchdog in ms (default 1000)");

/* ---- hardware fence ---- */
static const char *vc8k_fence_driver_name(struct dma_fence *f)
{
	return "vc8k";
}

static const char *vc8k_fence_timeline_name(struct dma_fence *f)
{
	return "vc8k-hw";
}

static const struct dma_fence_ops vc8k_fence_ops = {
	.get_driver_name = vc8k_fence_driver_name,
	.get_timeline_name = vc8k_fence_timeline_name,
};

static struct dma_fence *vc8k_hw_fence_create(struct vc8k_dev *v)
{
	struct dma_fence *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return ERR_PTR(-ENOMEM);
	dma_fence_init(f, &vc8k_fence_ops, &v->hw_lock, v->fence_ctx,
		       ++v->fence_seqno);
	return f;
}

/* ---- job lifetime ---- */
static void vc8k_job_release(struct kref *kref)
{
	struct vc8k_job *job = container_of(kref, struct vc8k_job, refcount);
	u32 i;

	for (i = 0; i < job->nr_bos; i++)
		drm_gem_object_put(job->bos[i]);
	kvfree(job->bos);
	dma_fence_put(job->hw_fence);
	dma_fence_put(job->done);
	kfree(job);
}

void vc8k_job_put(struct vc8k_job *job)
{
	kref_put(&job->refcount, vc8k_job_release);
}

/* ---- scheduler ops ---- */
static struct dma_fence *vc8k_run_job(struct drm_sched_job *sched_job)
{
	struct vc8k_job *job = to_vc8k_job(sched_job);
	struct vc8k_dev *v = job->vdev;
	struct dma_fence *fence;
	unsigned long flags;

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	fence = vc8k_hw_fence_create(v);
	if (IS_ERR(fence))
		return NULL;

	spin_lock_irqsave(&v->hw_lock, flags);
	job->hw_fence = dma_fence_get(fence);
	job->dec_done = job->pp_done = false;
	job->result = VC8K_RESULT_CANCELED;
	v->cur_job = job;
	vc8k_hw_run(v, job);
	spin_unlock_irqrestore(&v->hw_lock, flags);

	return fence;
}

static enum drm_gpu_sched_stat vc8k_timedout_job(struct drm_sched_job *sched_job)
{
	struct vc8k_job *job = to_vc8k_job(sched_job);
	struct vc8k_dev *v = job->vdev;
	unsigned long flags;
	int i;

	/* The interrupt may have won the race. */
	if (dma_fence_is_signaled(job->hw_fence))
		return DRM_GPU_SCHED_STAT_NOMINAL;

	drm_sched_stop(&v->sched, sched_job);

	dev_warn(v->dev, "job timed out (reg1=%08x reg60=%08x)\n",
		 vc8k_rd(v, VC8K_REG_DEC_CTRL), vc8k_rd(v, VC8K_REG_PP_CTRL));
	atomic_inc(&v->timeout_count);

	spin_lock_irqsave(&v->hw_lock, flags);
	/* abort, wait for idle, clear registers */
	vc8k_hw_reset(v);
	for (i = 0; i < VC8K_NUM_REGS; i++)
		job->snap[i] = 0;
	job->result = VC8K_RESULT_HW_TIMEOUT;
	if (v->cur_job == job)
		v->cur_job = NULL;
	dma_fence_set_error(job->hw_fence, -ETIMEDOUT);
	dma_fence_signal_locked(job->hw_fence);
	spin_unlock_irqrestore(&v->hw_lock, flags);

	/* Fence is already signalled: start() completes the job, no resubmit. */
	drm_sched_start(&v->sched, true);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void vc8k_free_job(struct drm_sched_job *sched_job)
{
	struct vc8k_job *job = to_vc8k_job(sched_job);

	drm_sched_job_cleanup(sched_job);
	vc8k_job_put(job);
}

static const struct drm_sched_backend_ops vc8k_sched_ops = {
	.run_job = vc8k_run_job,
	.timedout_job = vc8k_timedout_job,
	.free_job = vc8k_free_job,
};

int vc8k_sched_init(struct vc8k_dev *v)
{
	spin_lock_init(&v->hw_lock);
	v->fence_ctx = dma_fence_context_alloc(1);

	return drm_sched_init(&v->sched, &vc8k_sched_ops, 1, 0,
			      msecs_to_jiffies(vc8k_timeout_ms), NULL, NULL,
			      "vc8k", v->dev);
}

void vc8k_sched_fini(struct vc8k_dev *v)
{
	drm_sched_fini(&v->sched);
}
