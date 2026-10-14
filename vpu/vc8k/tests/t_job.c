/* P3: SUBMIT/WAIT validation, watchdog/recovery, fences, concurrency, kill. */
#include "vc8k_test.h"
#include <sys/wait.h>

#define SEC 1000000000ull

static int strict_mode(void)
{
	char c = '1';
	FILE *f = fopen("/sys/module/vc8k/parameters/reloc_strict", "r");
	if (f) { if (fscanf(f, " %c", &c) != 1) c = '1'; fclose(f); }
	return c != '0' && c != 'N';
}

/* Runs one garbage job to completion; returns wait result or -1. */
static int run_garbage(int fd, struct bo *s, struct bo *o, uint32_t *status)
{
	struct job j; struct drm_vc8k_wait w; uint32_t id;
	job_garbage(&j, s, o);
	if (job_submit(fd, &j, &id)) return -1;
	if (job_wait(fd, id, 10 * SEC, &w, NULL)) return -1;
	if (status) *status = w.dec_status;
	return w.result;
}

static void test_validation(int fd, struct bo *s, struct bo *o)
{
	struct job j; uint32_t id;
	struct drm_vc8k_wait w;

	TEST_BEGIN("submit validation");
	job_garbage(&j, s, o); j.flags = 0;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "flags == 0");

	job_garbage(&j, s, o); j.flags = 0x80;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "unknown flags");

	job_garbage(&j, s, o); j.nr_bos = VC8K_MAX_BOS + 1;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "too many bos");

	job_garbage(&j, s, o); j.bos[0].handle = 0x1234;
	CHECK_ERRNO(job_submit(fd, &j, &id), ENOENT, "bad bo handle");

	job_garbage(&j, s, o); j.bos[0].flags = 0;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "bo without access flags");

	job_garbage(&j, s, o); j.relocs[0].reg = 5;	/* not an address register */
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "reloc on non-address reg");

	job_garbage(&j, s, o); j.relocs[0].reg = VC8K_NUM_REGS;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "reloc reg out of range");

	job_garbage(&j, s, o); j.relocs[0].reg = 66;	/* PP reg in a DEC-only job */
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "PP reloc in DEC job");

	job_garbage(&j, s, o); j.relocs[0].bo_idx = 9;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "bo_idx out of range");

	job_garbage(&j, s, o); j.relocs[0].offset = s->size;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "reloc offset == bo size");

	job_garbage(&j, s, o); j.relocs[0].offset = ~0ull;
	CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "reloc offset huge");

	if (strict_mode()) {
		job_garbage(&j, s, o); j.regs[14] = 0x8a800000;	/* raw bus address */
		CHECK_ERRNO(job_submit(fd, &j, &id), EINVAL, "raw address in strict mode");
	} else {
		printf("  (reloc_strict=0, skipping raw-address check)\n");
	}

	job_garbage(&j, s, o);
	CHECK_ERRNO(vc8k_ioc(fd, DRM_IOCTL_VC8K_SUBMIT, &(struct drm_vc8k_submit){ .flags = 1, .regs = 0 }), EFAULT, "NULL regs pointer");

	CHECK_ERRNO(job_wait(fd, 999, 0, &w, NULL), ENOENT, "wait on unknown job");
	TEST_END("submit validation");
}

static void test_garbage_completes(int fd, struct bo *s, struct bo *o)
{
	struct job j; struct drm_vc8k_wait w; uint32_t id, regs[VC8K_NUM_REGS];
	uint64_t t0;
	int r;

	TEST_BEGIN("garbage job terminates, job id released");
	job_garbage(&j, s, o);
	CHECK(!job_submit(fd, &j, &id), "submit: %s", strerror(errno));
	CHECK(id > 0, "job id");
	t0 = now_ns();
	r = job_wait(fd, id, 10 * SEC, &w, regs);
	CHECK(!r, "wait: %s", strerror(errno));
	printf("  result=%u dec_status=0x%08x after %.1f ms\n", w.result, w.dec_status,
	       (now_ns() - t0) / 1e6);
	CHECK(w.result == VC8K_RESULT_OK || w.result == VC8K_RESULT_HW_TIMEOUT, "result %u", w.result);
	if (w.result == VC8K_RESULT_OK)
		CHECK(w.dec_status & VC8K_STATUS_IRQ, "OK result carries IRQ bit");
	CHECK_ERRNO(job_wait(fd, id, 0, &w, NULL), ENOENT, "second wait");
	TEST_END("garbage job terminates, job id released");
}

static void test_recovery(int fd, struct bo *s, struct bo *o)
{
	int i, bad = 0, to = 0;

	TEST_BEGIN("recovery: 50 sequential garbage jobs");
	for (i = 0; i < 50; i++) {
		int r = run_garbage(fd, s, o, NULL);
		if (r < 0) bad++;
		if (r == VC8K_RESULT_HW_TIMEOUT) to++;
	}
	printf("  %d ok, %d watchdog resets, %d failed\n", 50 - bad, to, bad);
	CHECK(!bad, "%d jobs failed to complete", bad);
	TEST_END("recovery: 50 sequential garbage jobs");
}

static void test_nonblocking_and_queue(int fd, struct bo *s, struct bo *o)
{
	enum { N = 8 };
	struct job j; uint32_t id[N]; int i, done = 0;

	TEST_BEGIN("queued jobs + poll wait");
	for (i = 0; i < N; i++) {
		job_garbage(&j, s, o);
		CHECK(!job_submit(fd, &j, &id[i]), "submit %d", i);
	}
	for (i = 0; i < N; i++) {
		struct drm_vc8k_wait w;
		int r = job_wait(fd, id[i], 0, &w, NULL);		/* poll */
		if (r && errno == ETIMEDOUT) r = job_wait(fd, id[i], 20 * SEC, &w, NULL);
		if (!r) done++;
	}
	CHECK(done == N, "%d of %d completed", done, N);
	TEST_END("queued jobs + poll wait");
}

static void test_syncobj(int fd, struct bo *s, struct bo *o)
{
	struct drm_syncobj_create c = { 0 };
	struct drm_syncobj_wait sw = { 0 };
	struct drm_syncobj_destroy d;
	struct job j; uint32_t id, handles[1];

	TEST_BEGIN("out_syncobj");
	CHECK(!vc8k_ioc(fd, DRM_IOCTL_SYNCOBJ_CREATE, &c), "syncobj create");
	job_garbage(&j, s, o);
	j.out_syncobj = c.handle;
	CHECK(!job_submit(fd, &j, &id), "submit: %s", strerror(errno));
	handles[0] = c.handle;
	sw.handles = (uintptr_t)handles; sw.count_handles = 1;
	sw.timeout_nsec = now_ns() + 10 * SEC;
	sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	CHECK(!vc8k_ioc(fd, DRM_IOCTL_SYNCOBJ_WAIT, &sw), "syncobj wait: %s", strerror(errno));
	CHECK(!job_wait(fd, id, 0, NULL, NULL), "job already done after fence: %s", strerror(errno));

	/* in_syncobj chaining: job B waits for A's fence */
	job_garbage(&j, s, o); j.in_syncobj = c.handle;
	CHECK(!job_submit(fd, &j, &id), "submit with in_syncobj");
	CHECK(!job_wait(fd, id, 10 * SEC, NULL, NULL), "wait chained");

	/* unknown syncobj */
	job_garbage(&j, s, o); j.out_syncobj = 0x5555;
	CHECK_ERRNO(job_submit(fd, &j, &id), ENOENT, "unknown out_syncobj");
	d.handle = c.handle; d.pad = 0;
	vc8k_ioc(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
	TEST_END("out_syncobj");
}

static int child_hammer(int iters)
{
	int fd = vc8k_open(), i, bad = 0;
	struct bo s, o;
	if (fd < 0 || bo_new(fd, 1 << 16, &s) || bo_new(fd, 1 << 20, &o)) return 3;
	for (i = 0; i < iters; i++)
		if (run_garbage(fd, &s, &o, NULL) < 0) bad++;
	return bad ? 1 : 0;
}

static void test_concurrent(int fd)
{
	enum { P = 4 };
	pid_t pid[P]; int i, st, bad = 0;
	(void)fd;
	TEST_BEGIN("concurrent clients (4 procs x 30 jobs)");
	for (i = 0; i < P; i++) {
		pid[i] = fork();
		if (!pid[i]) _exit(child_hammer(30));
	}
	for (i = 0; i < P; i++) {
		waitpid(pid[i], &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st)) bad++;
	}
	CHECK(!bad, "%d clients failed", bad);
	TEST_END("concurrent clients (4 procs x 30 jobs)");
}

static void test_kill_midjob(int fd, struct bo *s, struct bo *o)
{
	int round;

	TEST_BEGIN("kill -9 with jobs in flight");
	for (round = 0; round < 10; round++) {
		pid_t pid = fork();
		if (!pid) {
			int cfd = vc8k_open(), i;
			struct bo cs, co; struct job j; uint32_t id;
			if (cfd < 0 || bo_new(cfd, 1 << 16, &cs) || bo_new(cfd, 1 << 20, &co)) _exit(3);
			for (i = 0; i < 16; i++) { job_garbage(&j, &cs, &co); job_submit(cfd, &j, &id); }
			raise(SIGKILL);
			_exit(0);
		} else {
			int st;
			waitpid(pid, &st, 0);
		}
		/* device must still work right away */
		CHECK(run_garbage(fd, s, o, NULL) >= 0, "round %d: device dead after kill", round);
	}
	TEST_END("kill -9 with jobs in flight");
}

int main(void)
{
	struct bo s, o;
	int fd = vc8k_open();

	if (fd < 0) { printf("cannot open vc8k render node: %s\n", strerror(errno)); return 2; }
	if (bo_new(fd, 1 << 16, &s) || bo_new(fd, 1 << 20, &o)) { printf("bo alloc failed\n"); return 2; }
	memset(bo_map(fd, &s), 0, s.size);

	test_validation(fd, &s, &o);
	test_garbage_completes(fd, &s, &o);
	test_recovery(fd, &s, &o);
	test_nonblocking_and_queue(fd, &s, &o);
	test_syncobj(fd, &s, &o);
	test_concurrent(fd);
	test_kill_midjob(fd, &s, &o);

	/* final sanity: still alive */
	CHECK(run_garbage(fd, &s, &o, NULL) >= 0, "device alive at end");
	bo_free(fd, &s); bo_free(fd, &o);
	close(fd);
	printf("check dmesg for splats / 'job timed out' counts\n");
	return t_summary();
}
