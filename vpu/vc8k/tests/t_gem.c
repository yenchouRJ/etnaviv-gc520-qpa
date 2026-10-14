/* P2: GEM allocation, mmap, PRIME round trip, exhaustion, error paths. */
#include "vc8k_test.h"
#include <sys/wait.h>

static void test_basic(int fd)
{
	struct bo bo;
	uint32_t *p;
	uint64_t i;

	TEST_BEGIN("gem basic + mmap");
	CHECK(!bo_new(fd, 5000, &bo), "gem_new");
	CHECK(bo.size == 8192 || bo.size == 5000 + (4096 - 5000 % 4096) % 4096 || bo.size >= 5000, "size %llu", (unsigned long long)bo.size);
	CHECK(bo.dev_addr && !(bo.dev_addr & 0xfff), "dev_addr 0x%llx page aligned", (unsigned long long)bo.dev_addr);
	CHECK(bo.dev_addr < (1ull << 32), "dev_addr fits 32 bit");
	p = bo_map(fd, &bo);
	CHECK(p != NULL, "mmap");
	if (p) {
		for (i = 0; i < bo.size / 4; i++) p[i] = i * 2654435761u;
		for (i = 0; i < bo.size / 4; i++) if (p[i] != (uint32_t)(i * 2654435761u)) break;
		CHECK(i == bo.size / 4, "readback mismatch at word %llu", (unsigned long long)i);
	}
	bo_free(fd, &bo);
	TEST_END("gem basic + mmap");
}

static void test_errors(int fd)
{
	struct drm_vc8k_gem_new n = { 0 };
	struct drm_vc8k_gem_info i = { .handle = 0x7777 };
	struct bo bo;

	TEST_BEGIN("gem error paths");
	CHECK_ERRNO(vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_NEW, &n), EINVAL, "size 0");
	n.size = 4096; n.flags = 1;
	CHECK_ERRNO(vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_NEW, &n), EINVAL, "bad flags");
	n.flags = 0; n.size = 1ull << 40;
	CHECK_ERRNO(vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_NEW, &n), EINVAL, "huge size");
	CHECK_ERRNO(vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_INFO, &i), ENOENT, "info bad handle");
	CHECK(!bo_new(fd, 4096, &bo), "new");
	{
		uint32_t h = bo.handle;
		bo_free(fd, &bo);
		bo.handle = h;
		CHECK_ERRNO(vc8k_ioc(fd, DRM_IOCTL_VC8K_GEM_INFO, &(struct drm_vc8k_gem_info){ .handle = h }), ENOENT, "info after close");
		bo.handle = 0;
	}
	TEST_END("gem error paths");
}

static void test_prime(int fd)
{
	struct bo a, b;
	int pfd = -1, fd2;
	uint32_t h2 = 0;
	uint8_t *pa, *pb;

	TEST_BEGIN("prime export/import");
	CHECK(!bo_new(fd, 1 << 20, &a), "new");
	pa = bo_map(fd, &a);
	if (pa) memset(pa, 0xA5, a.size);
	CHECK(!bo_export(fd, a.handle, &pfd), "export");

	/* same-device re-import returns the same object (same address) */
	CHECK(!bo_import(fd, pfd, &h2), "import same fd");
	CHECK(!bo_info(fd, h2, &b), "info imported");
	CHECK(b.dev_addr == a.dev_addr, "same-device import dev_addr");

	/* mmap the dmabuf fd directly */
	pb = mmap(NULL, a.size, PROT_READ, MAP_SHARED, pfd, 0);
	CHECK(pb != MAP_FAILED, "dmabuf mmap");
	if (pb != MAP_FAILED) {
		CHECK(pb[0] == 0xA5 && pb[a.size - 1] == 0xA5, "dmabuf sees CPU writes");
		munmap(pb, a.size);
	}

	/* second open file: import must give a usable, contiguous handle */
	fd2 = vc8k_open();
	if (fd2 >= 0) {
		struct bo c;
		CHECK(!bo_import(fd2, pfd, &h2), "import in second file");
		CHECK(!bo_info(fd2, h2, &c), "info in second file");
		CHECK(c.dev_addr == a.dev_addr && c.size == a.size, "second-file view matches");
		c.handle = h2;
		pb = bo_map(fd2, &c);
		CHECK(pb && pb[12345] == 0xA5, "second-file mmap content");
		bo_free(fd2, &c);
		close(fd2);
	}
	/* export outlives the handle */
	bo_free(fd, &a);
	pb = mmap(NULL, 4096, PROT_READ, MAP_SHARED, pfd, 0);
	CHECK(pb != MAP_FAILED && pb[0] == 0xA5, "dmabuf survives handle close");
	if (pb != MAP_FAILED) munmap(pb, 4096);
	close(pfd);
	TEST_END("prime export/import");
}

static void test_exhaust(int fd)
{
	enum { MAXN = 4096 };
	static struct bo bos[MAXN];
	const uint64_t chunk = 4ull << 20;
	int n = 0, i;

	TEST_BEGIN("exhaustion + leak check");
	for (;;) {
		if (n == MAXN || bo_new(fd, chunk, &bos[n])) break;
		n++;
	}
	printf("  allocated %d x 4MiB = %d MiB before %s\n", n, n * 4, n == MAXN ? "cap" : strerror(errno));
	CHECK(n > 0, "at least one 4 MiB buffer");
	CHECK(n == MAXN || errno == ENOMEM, "exhaustion returns ENOMEM (got %s)", strerror(errno));
	for (i = 0; i < n; i++) bo_free(fd, &bos[i]);
	/* everything must be available again */
	for (i = 0; i < n; i++) {
		if (bo_new(fd, chunk, &bos[i])) break;
	}
	CHECK(i == n, "re-allocation after free: %d of %d (fragmentation/leak)", i, n);
	while (i--) bo_free(fd, &bos[i]);
	TEST_END("exhaustion + leak check");
}

static void test_process_death(int fd)
{
	pid_t pid = fork();
	int st, i;
	struct bo b;

	TEST_BEGIN("process death frees memory");
	if (pid == 0) {
		int cfd = vc8k_open();
		struct bo x;
		for (i = 0; i < 16; i++) bo_new(cfd, 4 << 20, &x);
		raise(SIGKILL);
		_exit(0);
	}
	waitpid(pid, &st, 0);
	usleep(100000);
	for (i = 0; i < 16; i++)
		if (bo_new(fd, 4 << 20, &b)) break; else bo_free(fd, &b);
	CHECK(i == 16, "alloc after killed process, iteration %d", i);
	TEST_END("process death frees memory");
}

int main(int argc, char **argv)
{
	int fd = vc8k_open();
	int exhaust = argc > 1 && !strcmp(argv[1], "--exhaust");

	if (fd < 0) { printf("cannot open vc8k render node: %s\n", strerror(errno)); return 2; }
	test_basic(fd);
	test_errors(fd);
	test_prime(fd);
	test_process_death(fd);
	if (exhaust) test_exhaust(fd);
	else printf("(skip exhaustion test; pass --exhaust to run)\n");
	close(fd);
	return t_summary();
}
