// SPDX-License-Identifier: GPL-2.0-only
/* SUBMIT / WAIT ioctls: validation, relocation, implicit fencing. */
#include <linux/dma-resv.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include <drm/drm_syncobj.h>
#include <drm/drm_gem.h>

#include "vc8k_drv.h"

#define VC8K_MAX_OUTSTANDING	256

unsigned int vc8k_reloc_strict = 1;
module_param_named(reloc_strict, vc8k_reloc_strict, uint, 0644);
MODULE_PARM_DESC(reloc_strict,
		 "Reject non-zero address registers that have no relocation (default 1)");

/*
 * Registers whose full 32 bits are a DMA bus address (HWIF_*_BASE in the
 * vendor register table 8170table.h).  Several fields share a register.
 */
static const u8 vc8k_addr_regs[] = {
	9, 10, 11, 12, 13,				/* mb ctrl .. dec out */
	14, 15, 16, 17, 18, 19, 20, 21, 22, 23,		/* refer0..9 / jpg out */
	24, 25, 26, 27, 28, 29,				/* refer10..15 */
	40, 41, 42, 43, 59,				/* qtable, dir mv, pjpeg, ch8pix */
	63, 64, 65, 66, 67, 73, 74, 93, 94,		/* PP in/out/blend */
};

bool vc8k_reg_is_address(u32 reg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc8k_addr_regs); i++)
		if (vc8k_addr_regs[i] == reg)
			return true;
	return false;
}

/* Address regs belong to an engine the job may not even run. */
static bool vc8k_reg_in_job(u32 reg, u32 flags)
{
	if (reg <= VC8K_DEC_LAST_REG)
		return flags & VC8K_SUBMIT_DEC;
	return flags & VC8K_SUBMIT_PP;
}

static int vc8k_apply_relocs(struct vc8k_dev *v, struct vc8k_job *job,
			     const struct drm_vc8k_submit_reloc *relocs,
			     u32 nr)
{
	u64 covered_lo = 0, covered_hi = 0;	/* bitmaps of regs 0..63, 64..127 */
	u32 i;

	for (i = 0; i < nr; i++) {
		const struct drm_vc8k_submit_reloc *r = &relocs[i];
		struct drm_gem_object *bo;
		u64 addr;

		if (r->reg >= VC8K_NUM_REGS || r->bo_idx >= job->nr_bos)
			return -EINVAL;
		if (!vc8k_reg_is_address(r->reg) ||
		    !vc8k_reg_in_job(r->reg, job->flags))
			return -EINVAL;

		bo = job->bos[r->bo_idx];
		if (r->offset >= bo->size)
			return -EINVAL;

		addr = vc8k_gem_dev_addr(bo) + r->offset;
		if (addr >> v->dma_bits)
			return -EINVAL;

		job->regs[r->reg] = (u32)addr;
		if (r->reg < 64)
			covered_lo |= BIT_ULL(r->reg);
		else
			covered_hi |= BIT_ULL(r->reg - 64);
	}

	if (!vc8k_reloc_strict)
		return 0;

	for (i = 0; i < ARRAY_SIZE(vc8k_addr_regs); i++) {
		u32 reg = vc8k_addr_regs[i];
		bool cov = reg < 64 ? covered_lo & BIT_ULL(reg) :
				      covered_hi & BIT_ULL(reg - 64);

		if (!vc8k_reg_in_job(reg, job->flags))
			continue;
		if (!cov && job->regs[reg]) {
			dev_dbg(v->dev, "reg %u has raw address 0x%08x\n",
				reg, job->regs[reg]);
			return -EINVAL;
		}
	}
	return 0;
}

int vc8k_ioctl_submit(struct drm_device *dev, void *data,
		      struct drm_file *file)
{
	struct vc8k_dev *v = to_vc8k(dev);
	struct vc8k_file *vf = file->driver_priv;
	struct drm_vc8k_submit *args = data;
	struct drm_vc8k_submit_bo *ubos = NULL;
	struct drm_vc8k_submit_reloc *relocs = NULL;
	struct drm_syncobj *out_sync = NULL;
	struct vc8k_job *job;
	struct ww_acquire_ctx acq;
	u32 i;
	int ret, id;

	if (args->pad || args->pad2 || (args->flags & ~VC8K_SUBMIT_FLAGS_MASK) ||
	    !args->flags || args->nr_bos > VC8K_MAX_BOS ||
	    args->nr_relocs > VC8K_MAX_RELOCS)
		return -EINVAL;

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	kref_init(&job->refcount);
	job->result = VC8K_RESULT_CANCELED;
	job->vdev = v;
	job->flags = args->flags;

	if (copy_from_user(job->regs, u64_to_user_ptr(args->regs),
			   sizeof(job->regs))) {
		ret = -EFAULT;
		goto err_free;
	}

	ubos = kvmalloc_array(max(args->nr_bos, 1u), sizeof(*ubos), GFP_KERNEL);
	relocs = kvmalloc_array(max(args->nr_relocs, 1u), sizeof(*relocs),
				GFP_KERNEL);
	job->bos = kvcalloc(max(args->nr_bos, 1u), sizeof(*job->bos),
			    GFP_KERNEL);
	if (!ubos || !relocs || !job->bos) {
		ret = -ENOMEM;
		goto err_free;
	}
	if (copy_from_user(ubos, u64_to_user_ptr(args->bos),
			   args->nr_bos * sizeof(*ubos)) ||
	    copy_from_user(relocs, u64_to_user_ptr(args->relocs),
			   args->nr_relocs * sizeof(*relocs))) {
		ret = -EFAULT;
		goto err_free;
	}

	for (i = 0; i < args->nr_bos; i++) {
		if (!ubos[i].flags || (ubos[i].flags & ~(VC8K_BO_READ | VC8K_BO_WRITE))) {
			ret = -EINVAL;
			goto err_free;
		}
		job->bos[i] = drm_gem_object_lookup(file, ubos[i].handle);
		if (!job->bos[i]) {
			ret = -ENOENT;
			goto err_free;
		}
		job->nr_bos = i + 1;
	}

	ret = vc8k_apply_relocs(v, job, relocs, args->nr_relocs);
	if (ret)
		goto err_free;

	if (args->out_syncobj) {
		out_sync = drm_syncobj_find(file, args->out_syncobj);
		if (!out_sync) {
			ret = -ENOENT;
			goto err_free;
		}
	}

	/* Reserve the id first so nothing can fail after the job is armed. */
	mutex_lock(&vf->job_lock);
	id = idr_alloc(&vf->jobs, NULL, 1, VC8K_MAX_OUTSTANDING + 1,
		       GFP_KERNEL);
	mutex_unlock(&vf->job_lock);
	if (id < 0) {
		ret = id == -ENOSPC ? -EBUSY : id;
		goto err_free;
	}

	ret = drm_sched_job_init(&job->base, &vf->entity, vf);
	if (ret)
		goto err_id;

	if (args->in_syncobj) {
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file,
							   args->in_syncobj, 0);
		if (ret)
			goto err_cleanup;
	}

	if (job->nr_bos) {
		ret = drm_gem_lock_reservations(job->bos, job->nr_bos, &acq);
		if (ret)
			goto err_cleanup;

		for (i = 0; i < job->nr_bos; i++) {
			bool write = ubos[i].flags & VC8K_BO_WRITE;

			ret = dma_resv_reserve_fences(job->bos[i]->resv, 1);
			if (!ret)
				ret = drm_sched_job_add_implicit_dependencies(
					&job->base, job->bos[i], write);
			if (ret) {
				drm_gem_unlock_reservations(job->bos,
							    job->nr_bos, &acq);
				goto err_cleanup;
			}
		}
	}

	drm_sched_job_arm(&job->base);
	job->done = dma_fence_get(&job->base.s_fence->finished);

	for (i = 0; i < job->nr_bos; i++)
		dma_resv_add_fence(job->bos[i]->resv, job->done,
				   (ubos[i].flags & VC8K_BO_WRITE) ?
				   DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
	if (job->nr_bos)
		drm_gem_unlock_reservations(job->bos, job->nr_bos, &acq);

	if (out_sync)
		drm_syncobj_replace_fence(out_sync, job->done);

	kref_get(&job->refcount);	/* held by the idr */
	mutex_lock(&vf->job_lock);
	idr_replace(&vf->jobs, job, id);
	mutex_unlock(&vf->job_lock);

	args->job_id = id;
	drm_sched_entity_push_job(&job->base);	/* sched owns the initial ref */
	ret = 0;
	goto out;

err_cleanup:
	drm_sched_job_cleanup(&job->base);
err_id:
	mutex_lock(&vf->job_lock);
	idr_remove(&vf->jobs, id);
	mutex_unlock(&vf->job_lock);
err_free:
	vc8k_job_put(job);
out:
	if (out_sync)
		drm_syncobj_put(out_sync);
	kvfree(ubos);
	kvfree(relocs);
	return ret;
}

int vc8k_ioctl_wait(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct vc8k_file *vf = file->driver_priv;
	struct drm_vc8k_wait *args = data;
	struct vc8k_job *job;
	u64 ns = min_t(u64, args->timeout_ns, 3600ull * NSEC_PER_SEC);
	long t;
	int ret = 0;

	mutex_lock(&vf->job_lock);
	job = idr_find(&vf->jobs, args->job_id);
	if (job)
		kref_get(&job->refcount);
	mutex_unlock(&vf->job_lock);
	if (!job)
		return -ENOENT;

	t = dma_fence_wait_timeout(job->done, true, nsecs_to_jiffies64(ns));
	if (t < 0) {
		ret = t;
		goto out;
	}
	if (t == 0) {
		ret = -ETIMEDOUT;
		goto out;
	}

	args->result = job->result;
	args->dec_status = job->snap[VC8K_REG_DEC_CTRL];
	args->pp_status = job->snap[VC8K_REG_PP_CTRL];
	if (args->regs &&
	    copy_to_user(u64_to_user_ptr(args->regs), job->snap,
			 sizeof(job->snap))) {
		ret = -EFAULT;
		goto out;
	}

	mutex_lock(&vf->job_lock);
	if (idr_find(&vf->jobs, args->job_id) == job) {
		idr_remove(&vf->jobs, args->job_id);
		vc8k_job_put(job);	/* the idr's reference */
	}
	mutex_unlock(&vf->job_lock);
out:
	vc8k_job_put(job);
	return ret;
}
