/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR MIT */
/*
 * Nuvoton MA35 VC8000NanoD (Verisilicon/Hantro) decoder + post-processor
 * DRM render-node uAPI.
 *
 * The kernel provides memory (contiguous GEM/dma-buf), register-job
 * submission, interrupts and fences.  All codec logic lives in userspace.
 * Structures are append-only; bump VC8K_UAPI_VERSION on incompatible changes.
 */
#ifndef __VC8K_DRM_H__
#define __VC8K_DRM_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define VC8K_UAPI_VERSION	1

/*
 * Hardware register file exchanged with the kernel: 32-bit software registers
 * 0..100.  0..59 belong to the decoder, 60..100 to the post-processor (PP).
 * Register N lives at MMIO offset N * 4.
 */
#define VC8K_NUM_REGS		101
#define VC8K_DEC_FIRST_REG	0
#define VC8K_DEC_LAST_REG	59
#define VC8K_PP_FIRST_REG	60
#define VC8K_PP_LAST_REG	100

/* Interrupt/status register bits (reg 1 for decoder, reg 60 for PP). */
#define VC8K_STATUS_ENABLE	(1u << 0)
#define VC8K_STATUS_IRQ_DIS	(1u << 4)
#define VC8K_STATUS_ABORT	(1u << 5)
#define VC8K_STATUS_IRQ		(1u << 8)
#define VC8K_STATUS_RDY		(1u << 12)
#define VC8K_STATUS_BUS_ERR	(1u << 13)
#define VC8K_STATUS_BUF_EMPTY	(1u << 14)
#define VC8K_STATUS_ASO		(1u << 15)
#define VC8K_STATUS_ERROR	(1u << 16)
#define VC8K_STATUS_SLICE	(1u << 17)
#define VC8K_STATUS_TIMEOUT	(1u << 18)

/* GET_PARAM */
#define VC8K_PARAM_UAPI_VERSION	0
#define VC8K_PARAM_HW_ID	1	/* reg 0 */
#define VC8K_PARAM_SYNTH_CFG	2	/* reg 50 decoder synthesis config */
#define VC8K_PARAM_SYNTH_CFG_2	3	/* reg 54 */
#define VC8K_PARAM_FUSE_CFG	4	/* reg 57 */
#define VC8K_PARAM_PP_FUSE_CFG	5	/* reg 99 */
#define VC8K_PARAM_PP_SYNTH_CFG	6	/* reg 100 */
#define VC8K_PARAM_CLK_RATE_HZ	7
#define VC8K_PARAM_DMA_MASK_BITS 8
#define VC8K_PARAM_NUM_REGS	9

struct drm_vc8k_get_param {
	__u32 param;	/* in */
	__u32 pad;
	__u64 value;	/* out */
};

/* GEM_NEW: always physically contiguous, write-combined CPU mapping. */
struct drm_vc8k_gem_new {
	__u64 size;	/* in: bytes, rounded up to PAGE_SIZE */
	__u32 flags;	/* in: must be 0 */
	__u32 handle;	/* out */
};

/* GEM_INFO: also valid for imported (PRIME) handles. */
struct drm_vc8k_gem_info {
	__u32 handle;	/* in */
	__u32 pad;
	__u64 offset;	/* out: mmap() offset on the DRM fd */
	__u64 size;	/* out */
	__u64 dev_addr;	/* out: VPU bus address (informational) */
};

/*
 * SUBMIT: run one job on the VPU.
 *
 * regs:   pointer to VC8K_NUM_REGS u32 values.  The kernel writes them in the
 *         hardware-mandated order (PP first, decoder enable last).
 * bos:    buffers referenced by the job (each gets a reference, implicit
 *         fencing on dma_resv, lifetime = job).
 * relocs: address registers.  For every reloc the kernel overwrites
 *         regs[reg] = dev_addr(bos[bo_idx]) + offset.  With the module
 *         parameter reloc_strict=1 (default), any address register that is
 *         non-zero and has no reloc is rejected with -EINVAL.
 * flags:  which engines run.  DEC|PP == combined pipeline (post-processor
 *         fed straight from the decoder); the job completes on the decoder
 *         interrupt.
 */
#define VC8K_SUBMIT_DEC		(1u << 0)
#define VC8K_SUBMIT_PP		(1u << 1)
#define VC8K_SUBMIT_FLAGS_MASK	(VC8K_SUBMIT_DEC | VC8K_SUBMIT_PP)

#define VC8K_BO_READ		(1u << 0)
#define VC8K_BO_WRITE		(1u << 1)

struct drm_vc8k_submit_bo {
	__u32 handle;
	__u32 flags;	/* VC8K_BO_* */
};

struct drm_vc8k_submit_reloc {
	__u32 reg;	/* register index holding a bus address */
	__u32 bo_idx;	/* index into bos[] */
	__u64 offset;	/* byte offset inside the BO */
};

#define VC8K_MAX_BOS		64
#define VC8K_MAX_RELOCS		64

struct drm_vc8k_submit {
	__u32 flags;		/* VC8K_SUBMIT_* */
	__u32 nr_bos;
	__u32 nr_relocs;
	__u32 in_syncobj;	/* 0 = none; waited before running */
	__u32 out_syncobj;	/* 0 = none; signalled on completion */
	__u32 pad;
	__u64 regs;		/* -> u32[VC8K_NUM_REGS] */
	__u64 bos;		/* -> struct drm_vc8k_submit_bo[nr_bos] */
	__u64 relocs;		/* -> struct drm_vc8k_submit_reloc[nr_relocs] */
	__u32 job_id;		/* out: for VC8K_WAIT */
	__u32 pad2;
};

/*
 * WAIT: wait for a job to complete and fetch its result.
 * timeout_ns is a relative timeout (0 = poll).  Returns -ETIMEDOUT if the
 * job is still running (job stays valid, wait again).  On completion the job
 * id is released.
 */
#define VC8K_RESULT_OK		0	/* hardware raised its interrupt */
#define VC8K_RESULT_HW_TIMEOUT	1	/* kernel watchdog aborted the job */
#define VC8K_RESULT_CANCELED	2	/* scheduler dropped the job */

struct drm_vc8k_wait {
	__u32 job_id;		/* in */
	__u32 result;		/* out: VC8K_RESULT_* */
	__u64 timeout_ns;	/* in */
	__u32 dec_status;	/* out: reg 1 at interrupt time */
	__u32 pp_status;	/* out: reg 60 at interrupt time */
	__u64 regs;		/* in: -> u32[VC8K_NUM_REGS] to fill, or 0 */
};

#define DRM_VC8K_GET_PARAM	0x00
#define DRM_VC8K_GEM_NEW	0x01
#define DRM_VC8K_GEM_INFO	0x02
#define DRM_VC8K_SUBMIT		0x03
#define DRM_VC8K_WAIT		0x04

#define DRM_IOCTL_VC8K_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VC8K_GET_PARAM, struct drm_vc8k_get_param)
#define DRM_IOCTL_VC8K_GEM_NEW \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VC8K_GEM_NEW, struct drm_vc8k_gem_new)
#define DRM_IOCTL_VC8K_GEM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VC8K_GEM_INFO, struct drm_vc8k_gem_info)
#define DRM_IOCTL_VC8K_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VC8K_SUBMIT, struct drm_vc8k_submit)
#define DRM_IOCTL_VC8K_WAIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_VC8K_WAIT, struct drm_vc8k_wait)

#if defined(__cplusplus)
}
#endif

#endif /* __VC8K_DRM_H__ */
