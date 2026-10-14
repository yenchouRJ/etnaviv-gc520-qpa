# SPDX-License-Identifier: GPL-2.0-only
# In-tree: drivers/gpu/drm/vc8k/Makefile.  Out-of-tree: make -C $KDIR M=$PWD
vc8k-y := vc8k_drv.o vc8k_gem.o vc8k_hw.o vc8k_sched.o vc8k_submit.o
obj-$(CONFIG_DRM_VC8K) += vc8k.o

# out-of-tree (make M=...): default to a module, find the uAPI header
ifeq ($(CONFIG_DRM_VC8K),)
obj-m += vc8k.o
endif
ccflags-y += -I$(src)/../include/uapi -I$(srctree)/include/uapi/drm
