// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nuvoton MA35 VC8000NanoD (Hantro) video decoder - DRM render-node driver.
 *
 * Kernel side = contiguous GEM memory + register job submission + IRQ +
 * fences + reset.  Codec logic is userspace (libvc8k).
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_syncobj.h>

#include "vc8k_drv.h"

static int vc8k_ioctl_get_param(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct vc8k_dev *v = to_vc8k(dev);
	struct drm_vc8k_get_param *args = data;

	if (args->pad)
		return -EINVAL;

	switch (args->param) {
	case VC8K_PARAM_UAPI_VERSION:
		args->value = VC8K_UAPI_VERSION;
		break;
	case VC8K_PARAM_HW_ID:
		args->value = v->caps[0];
		break;
	case VC8K_PARAM_SYNTH_CFG:
		args->value = v->caps[50];
		break;
	case VC8K_PARAM_SYNTH_CFG_2:
		args->value = v->caps[54];
		break;
	case VC8K_PARAM_FUSE_CFG:
		args->value = v->caps[57];
		break;
	case VC8K_PARAM_PP_FUSE_CFG:
		args->value = v->caps[99];
		break;
	case VC8K_PARAM_PP_SYNTH_CFG:
		args->value = v->caps[100];
		break;
	case VC8K_PARAM_CLK_RATE_HZ:
		args->value = v->clk ? clk_get_rate(v->clk) : 0;
		break;
	case VC8K_PARAM_DMA_MASK_BITS:
		args->value = v->dma_bits;
		break;
	case VC8K_PARAM_NUM_REGS:
		args->value = VC8K_NUM_REGS;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct drm_ioctl_desc vc8k_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VC8K_GET_PARAM, vc8k_ioctl_get_param, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC8K_GEM_NEW, vc8k_ioctl_gem_new, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC8K_GEM_INFO, vc8k_ioctl_gem_info, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC8K_SUBMIT, vc8k_ioctl_submit, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC8K_WAIT, vc8k_ioctl_wait, DRM_RENDER_ALLOW),
};

static int vc8k_open(struct drm_device *dev, struct drm_file *file)
{
	struct vc8k_dev *v = to_vc8k(dev);
	struct vc8k_file *vf;
	int ret;

	vf = kzalloc(sizeof(*vf), GFP_KERNEL);
	if (!vf)
		return -ENOMEM;

	vf->vdev = v;
	mutex_init(&vf->job_lock);
	idr_init_base(&vf->jobs, 1);

	ret = drm_sched_entity_init(&vf->entity, DRM_SCHED_PRIORITY_NORMAL,
				    v->scheds, 1, NULL);
	if (ret) {
		kfree(vf);
		return ret;
	}

	file->driver_priv = vf;
	return 0;
}

static void vc8k_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct vc8k_file *vf = file->driver_priv;
	struct vc8k_job *job;
	int id;

	/* Waits for queued jobs (bounded by the watchdog), kills the rest. */
	drm_sched_entity_destroy(&vf->entity);

	idr_for_each_entry(&vf->jobs, job, id)
		vc8k_job_put(job);
	idr_destroy(&vf->jobs);
	mutex_destroy(&vf->job_lock);
	kfree(vf);
}

DEFINE_DRM_GEM_FOPS(vc8k_drm_fops);

static const struct drm_driver vc8k_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.open = vc8k_open,
	.postclose = vc8k_postclose,
	.ioctls = vc8k_ioctls,
	.num_ioctls = ARRAY_SIZE(vc8k_ioctls),
	.fops = &vc8k_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	.name = "vc8k",
	.desc = "Nuvoton VC8000NanoD decoder",
	.date = "20260101",
	.major = 1,
	.minor = 0,
};

static int vc8k_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vc8k_dev *v;
	int ret;

	v = devm_drm_dev_alloc(dev, &vc8k_drm_driver, struct vc8k_dev, ddev);
	if (IS_ERR(v))
		return PTR_ERR(v);
	v->dev = dev;
	platform_set_drvdata(pdev, v);

	v->dma_bits = 32;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(v->dma_bits));
	if (ret)
		return dev_err_probe(dev, ret, "cannot set 32-bit DMA mask\n");

	/* Optional dedicated pool (memory-region = <&vc8k_buf>). */
	ret = of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "reserved memory init\n");
	if (!ret)
		dev_info(dev, "using dedicated memory-region\n");

	v->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(v->mmio))
		return PTR_ERR(v->mmio);

	v->irq = platform_get_irq(pdev, 0);
	if (v->irq < 0)
		return v->irq;

	v->clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(v->clk))
		return dev_err_probe(dev, PTR_ERR(v->clk), "clock\n");

	v->rstc = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(v->rstc))
		return dev_err_probe(dev, PTR_ERR(v->rstc), "reset\n");

	vc8k_hw_reset(v);
	vc8k_hw_read_caps(v);
	dev_info(dev, "HW id 0x%08x synth 0x%08x/0x%08x pp 0x%08x clk %lu Hz\n",
		 v->caps[0], v->caps[50], v->caps[54], v->caps[100],
		 v->clk ? clk_get_rate(v->clk) : 0);

	ret = vc8k_sched_init(v);
	if (ret)
		return dev_err_probe(dev, ret, "scheduler init\n");
	v->scheds[0] = &v->sched;

	ret = devm_request_irq(dev, v->irq, vc8k_irq, 0, "vc8k", v);
	if (ret) {
		dev_err_probe(dev, ret, "request irq %d\n", v->irq);
		goto err_sched;
	}

	/* Register last: the device must be fully usable when visible. */
	ret = drm_dev_register(&v->ddev, 0);
	if (ret)
		goto err_sched;

	return 0;

err_sched:
	vc8k_sched_fini(v);
	return ret;
}

static int vc8k_remove(struct platform_device *pdev)
{
	struct vc8k_dev *v = platform_get_drvdata(pdev);

	drm_dev_unplug(&v->ddev);
	vc8k_sched_fini(v);
	vc8k_hw_reset(v);
	return 0;
}

static void vc8k_shutdown(struct platform_device *pdev)
{
	vc8k_hw_reset(platform_get_drvdata(pdev));
}

static const struct of_device_id vc8k_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-vc8k" },
	{ .compatible = "nuvoton,ma35d0-vc8k" },
	{ .compatible = "nuvoton,ma35h0-vc8k" },
	{ }
};
MODULE_DEVICE_TABLE(of, vc8k_of_match);

static struct platform_driver vc8k_platform_driver = {
	.probe = vc8k_probe,
	.remove = vc8k_remove,
	.shutdown = vc8k_shutdown,
	.driver = {
		.name = "vc8k-drm",
		.of_match_table = vc8k_of_match,
	},
};
module_platform_driver(vc8k_platform_driver);

MODULE_DESCRIPTION("Nuvoton MA35 VC8000NanoD DRM render-node driver");
MODULE_LICENSE("GPL");
