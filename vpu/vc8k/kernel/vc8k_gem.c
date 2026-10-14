// SPDX-License-Identifier: GPL-2.0-only
/* Contiguous GEM objects (CMA / reserved pool) with PRIME support. */
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_vma_manager.h>

#include "vc8k_drv.h"

#define VC8K_MAX_BO_SIZE	(512ull << 20)

u64 vc8k_gem_dev_addr(struct drm_gem_object *obj)
{
	return to_drm_gem_dma_obj(obj)->dma_addr;
}

int vc8k_ioctl_gem_new(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct drm_vc8k_gem_new *args = data;
	struct drm_gem_dma_object *dma;
	u32 handle;
	int ret;

	if (args->flags || !args->size || args->size > VC8K_MAX_BO_SIZE)
		return -EINVAL;

	dma = drm_gem_dma_create(dev, PAGE_ALIGN(args->size));
	if (IS_ERR(dma))
		return PTR_ERR(dma);

	ret = drm_gem_handle_create(file, &dma->base, &handle);
	drm_gem_object_put(&dma->base);
	if (ret)
		return ret;

	args->handle = handle;
	return 0;
}

int vc8k_ioctl_gem_info(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct drm_vc8k_gem_info *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (!ret) {
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
		args->size = obj->size;
		args->dev_addr = vc8k_gem_dev_addr(obj);
	}

	drm_gem_object_put(obj);
	return ret;
}
