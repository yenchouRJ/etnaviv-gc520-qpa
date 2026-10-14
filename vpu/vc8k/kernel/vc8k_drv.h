/* SPDX-License-Identifier: GPL-2.0-only */
/* VC8000NanoD DRM render-node driver (Nuvoton MA35) */
#ifndef VC8K_DRV_H
#define VC8K_DRV_H

#include <linux/clk.h>
#include <linux/dma-fence.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/gpu_scheduler.h>

#include <drm/vc8k_drm.h>

/* software register indices */
#define VC8K_REG_DEC_CTRL	1	/* enable/abort/irq/status */
#define VC8K_REG_DEC_CFG2	2
#define VC8K_REG_PP_CTRL	60
#define VC8K_REG_PP_RASTER	91	/* legacy: forced to 0 for raster output */

#define VC8K_DEC_CFG2_BIT7	0x80u	/* cleared by the legacy driver */

struct vc8k_job;

struct vc8k_dev {
	struct drm_device ddev;
	struct device *dev;
	void __iomem *mmio;
	struct clk *clk;
	struct reset_control *rstc;
	int irq;

	/* read-only capability registers sampled at probe time */
	u32 caps[VC8K_NUM_REGS];
	u32 dma_bits;

	/* scheduler + single hardware queue */
	struct drm_gpu_scheduler sched;
	struct drm_gpu_scheduler *scheds[1];
	u64 fence_ctx;
	u64 fence_seqno;
	spinlock_t hw_lock;		/* protects cur_job, fence, irq path */
	struct vc8k_job *cur_job;	/* job programmed into the hardware */

	atomic_t irq_count;
	atomic_t timeout_count;
};

struct vc8k_file {
	struct vc8k_dev *vdev;
	struct drm_sched_entity entity;
	struct mutex job_lock;
	struct idr jobs;
};

struct vc8k_job {
	struct drm_sched_job base;
	struct kref refcount;
	struct vc8k_dev *vdev;

	u32 flags;
	u32 regs[VC8K_NUM_REGS];
	u32 snap[VC8K_NUM_REGS];
	u32 dec_status, pp_status;
	u32 result;
	bool dec_done, pp_done;

	struct dma_fence *hw_fence;	/* signalled by irq/timeout */
	struct dma_fence *done;		/* == &base.s_fence->finished */

	struct drm_gem_object **bos;
	u32 nr_bos;
};

static inline struct vc8k_dev *to_vc8k(struct drm_device *d)
{
	return container_of(d, struct vc8k_dev, ddev);
}

static inline u32 vc8k_rd(struct vc8k_dev *v, u32 reg)
{
	return readl_relaxed(v->mmio + reg * 4);
}

static inline void vc8k_wr(struct vc8k_dev *v, u32 reg, u32 val)
{
	writel_relaxed(val, v->mmio + reg * 4);
}

/* vc8k_hw.c */
void vc8k_hw_reset(struct vc8k_dev *v);
void vc8k_hw_read_caps(struct vc8k_dev *v);
void vc8k_hw_run(struct vc8k_dev *v, struct vc8k_job *job);
irqreturn_t vc8k_irq(int irq, void *data);
extern unsigned int vc8k_reloc_strict;
extern unsigned int vc8k_pp_force_raster;
extern unsigned int vc8k_timeout_ms;

/* vc8k_sched.c */
int vc8k_sched_init(struct vc8k_dev *v);
void vc8k_sched_fini(struct vc8k_dev *v);
void vc8k_job_put(struct vc8k_job *job);
static inline struct vc8k_job *to_vc8k_job(struct drm_sched_job *j)
{
	return container_of(j, struct vc8k_job, base);
}

/* vc8k_gem.c */
int vc8k_ioctl_gem_new(struct drm_device *, void *, struct drm_file *);
int vc8k_ioctl_gem_info(struct drm_device *, void *, struct drm_file *);
u64 vc8k_gem_dev_addr(struct drm_gem_object *obj);

/* vc8k_submit.c */
int vc8k_ioctl_submit(struct drm_device *, void *, struct drm_file *);
int vc8k_ioctl_wait(struct drm_device *, void *, struct drm_file *);
bool vc8k_reg_is_address(u32 reg);

#endif
