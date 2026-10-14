/* Minimal libdrm-free helpers for the vc8k render-node tests. */
#ifndef VC8K_TEST_H
#define VC8K_TEST_H

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/vc8k_drm.h>	/* pulls in libdrm's drm.h */

/* ---- test bookkeeping ---- */
static int t_fail, t_pass;
#define CHECK(cond, ...) do {						\
	if (cond) { t_pass++; }						\
	else { t_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);	\
	       printf(__VA_ARGS__); printf("\n"); }			\
} while (0)
#define CHECK_ERRNO(call, want, what) do {				\
	int _r = (call); int _e = _r ? errno : 0;			\
	CHECK(_r == -1 && _e == (want), "%s: ret=%d errno=%d (%s), want %d (%s)", \
	      what, _r, _e, strerror(_e), want, strerror(want));	\
} while (0)
#define TEST_BEGIN(name) printf("[ RUN  ] %s\n", name)
#define TEST_END(name)  printf("[ %s ] %s\n", t_fail ? "FAIL" : " OK ", name)
static inline int t_summary(void)
{
	printf("\n%d checks passed, %d failed\n", t_pass, t_fail);
	return t_fail ? 1 : 0;
}

static inline uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- device ---- */
static inline int vc8k_ioc(int fd, unsigned long req, void *arg)
{
	int r;
	do { r = ioctl(fd, req, arg); } while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* Find the render node whose DRM driver name is "vc8k" ($VC8K_DEV overrides). */
static inline int vc8k_open(void)
{
	const char *env = getenv("VC8K_DEV");
	struct dirent *de;
	DIR *d;
	int fd;

	if (env)
		return open(env, O_RDWR | O_CLOEXEC);

	d = opendir("/dev/dri");
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		char path[300], name[32] = "";
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };

		if (strncmp(de->d_name, "renderD", 7))
			continue;
		snprintf(path, sizeof(path), "/dev/dri/%s", de->d_name);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!vc8k_ioc(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "vc8k")) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	closedir(d);
	errno = ENODEV;
	return -1;
}

static inline int vc8k_param(int fd, uint32_t param, uint64_t *val)
{
	struct drm_vc8k_get_param p = { .param = param };
	int r = vc8k_ioc(fd, DRM_IOCTL_VC8K_GET_PARAM, &p);
	*val = p.value;
	return r;
}

/* ---- buffers ---- */
struct bo {
	uint32_t handle;
	uint64_t size, offset, dev_addr;
	void *map;
};

static inline int bo_new(int fd, uint64_t size, struct bo *bo)
{
	struct drm_vc8k_gem_new n = { .size = size };
	struct drm_vc8k_gem_info i = { 0 };
	memset(bo, 0, sizeof(*bo));
	if (vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_NEW, &n))
		return -1;
	i.handle = n.handle;
	if (vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_INFO, &i))
		return -1;
	bo->handle = n.handle;
	bo->size = i.size;
	bo->offset = i.offset;
	bo->dev_addr = i.dev_addr;
	return 0;
}

static inline int bo_info(int fd, uint32_t handle, struct bo *bo)
{
	struct drm_vc8k_gem_info i = { .handle = handle };
	memset(bo, 0, sizeof(*bo));
	if (vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_INFO, &i))
		return -1;
	bo->handle = handle;
	bo->size = i.size;
	bo->offset = i.offset;
	bo->dev_addr = i.dev_addr;
	return 0;
}

static inline void *bo_map(int fd, struct bo *bo)
{
	if (!bo->map) {
		void *m = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			       fd, bo->offset);
		bo->map = m == MAP_FAILED ? NULL : m;
	}
	return bo->map;
}

static inline void bo_free(int fd, struct bo *bo)
{
	struct drm_gem_close c = { .handle = bo->handle };
	if (bo->map)
		munmap(bo->map, bo->size);
	if (bo->handle)
		vc8k_ioc(fd, DRM_IOCTL_GEM_CLOSE, &c);
	memset(bo, 0, sizeof(*bo));
}

static inline int bo_export(int fd, uint32_t handle, int *prime_fd)
{
	struct drm_prime_handle p = { .handle = handle, .flags = DRM_CLOEXEC | DRM_RDWR };
	if (vc8k_ioc(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &p))
		return -1;
	*prime_fd = p.fd;
	return 0;
}

static inline int bo_import(int fd, int prime_fd, uint32_t *handle)
{
	struct drm_prime_handle p = { .fd = prime_fd };
	if (vc8k_ioc(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p))
		return -1;
	*handle = p.handle;
	return 0;
}

/* ---- jobs ---- */
struct job {
	uint32_t flags;
	uint32_t regs[VC8K_NUM_REGS];
	struct drm_vc8k_submit_bo bos[VC8K_MAX_BOS];
	struct drm_vc8k_submit_reloc relocs[VC8K_MAX_RELOCS];
	uint32_t nr_bos, nr_relocs;
	uint32_t in_syncobj, out_syncobj;
};

/* returns index of the bo inside the job */
static inline int job_add_bo(struct job *j, uint32_t handle, uint32_t flags)
{
	uint32_t i;
	for (i = 0; i < j->nr_bos; i++)
		if (j->bos[i].handle == handle) {
			j->bos[i].flags |= flags;
			return i;
		}
	j->bos[j->nr_bos] = (struct drm_vc8k_submit_bo){ handle, flags };
	return j->nr_bos++;
}

static inline void job_reloc(struct job *j, uint32_t reg, uint32_t handle,
			     uint32_t bo_flags, uint64_t offset)
{
	int idx = job_add_bo(j, handle, bo_flags);
	j->relocs[j->nr_relocs++] = (struct drm_vc8k_submit_reloc){ reg, idx, offset };
	j->regs[reg] = 0;		/* kernel fills it */
}

static inline int job_submit(int fd, struct job *j, uint32_t *id)
{
	struct drm_vc8k_submit s = {
		.flags = j->flags, .nr_bos = j->nr_bos, .nr_relocs = j->nr_relocs,
		.in_syncobj = j->in_syncobj, .out_syncobj = j->out_syncobj,
		.regs = (uintptr_t)j->regs, .bos = (uintptr_t)j->bos,
		.relocs = (uintptr_t)j->relocs,
	};
	int r = vc8k_ioc(fd, DRM_IOCTL_VC8K_SUBMIT, &s);
	if (id)
		*id = s.job_id;
	return r;
}

static inline int job_wait(int fd, uint32_t id, uint64_t timeout_ns,
			   struct drm_vc8k_wait *out, uint32_t *regs)
{
	struct drm_vc8k_wait w = { .job_id = id, .timeout_ns = timeout_ns,
				   .regs = (uintptr_t)regs };
	int r = vc8k_ioc(fd, DRM_IOCTL_VC8K_WAIT, &w);
	if (out)
		*out = w;
	return r;
}

/*
 * A job the hardware cannot decode meaningfully (empty stream): it must end
 * with either an error/buffer-empty interrupt or the kernel watchdog, never a
 * hang.  Used for robustness/recovery tests.
 */
static inline void job_garbage(struct job *j, struct bo *stream, struct bo *out)
{
	memset(j, 0, sizeof(*j));
	j->flags = VC8K_SUBMIT_DEC;
	j->regs[2] = 1u << 23;			/* DEC_TIMEOUT_E */
	j->regs[1] = VC8K_STATUS_ENABLE;
	job_reloc(j, 12, stream->handle, VC8K_BO_READ, 0);	/* RLC_VLC_BASE */
	job_reloc(j, 13, out->handle, VC8K_BO_WRITE, 0);	/* DEC_OUT_BASE */
}

#endif
