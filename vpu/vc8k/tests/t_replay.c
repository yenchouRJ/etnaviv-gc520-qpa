/*
 * Register-image replay: run a captured decode/PP job (e.g. from the legacy
 * V4L2 driver trace) through the new driver and dump the output buffers.
 *
 *   t_replay --regs regs.bin --flags dec[,pp]
 *            --bo stream:65536:stream.bin --bo out:3110400
 *            --reloc 12=stream+0 --reloc 13=out+0
 *            [--set 3=0x10000000] --dump out:out.nv12[:len] [--timeout-ms 2000]
 *
 * regs.bin = VC8K_NUM_REGS little-endian u32 (register 0..100).  Address
 * registers must be given as --reloc so the kernel can validate them.
 */
#include "vc8k_test.h"

struct nbo { char name[32]; struct bo bo; };

static struct nbo *find(struct nbo *l, int n, const char *name)
{
	int i;
	for (i = 0; i < n; i++) if (!strcmp(l[i].name, name)) return &l[i];
	return NULL;
}

int main(int argc, char **argv)
{
	struct nbo bos[16]; int nb = 0;
	struct job j; struct drm_vc8k_wait w; uint32_t id, out[VC8K_NUM_REGS];
	const char *dumps[8]; int nd = 0, i, timeout_ms = 2000, fd;
	FILE *f;

	memset(&j, 0, sizeof(j));
	j.flags = VC8K_SUBMIT_DEC;
	fd = vc8k_open();
	if (fd < 0) { perror("open"); return 2; }

	/* pass 1: regs, flags, buffers */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--regs") && i + 1 < argc) {
			f = fopen(argv[++i], "rb");
			if (!f || fread(j.regs, 4, VC8K_NUM_REGS, f) != VC8K_NUM_REGS) { fprintf(stderr, "bad regs file\n"); return 2; }
			fclose(f);
		} else if (!strcmp(argv[i], "--flags") && i + 1 < argc) {
			j.flags = 0;
			if (strstr(argv[++i], "dec")) j.flags |= VC8K_SUBMIT_DEC;
			if (strstr(argv[i], "pp")) j.flags |= VC8K_SUBMIT_PP;
		} else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) {
			timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--bo") && i + 1 < argc) {
			char *a = strdup(argv[++i]), *sz, *file; struct nbo *n = &bos[nb++];
			sz = strchr(a, ':'); *sz++ = 0; file = strchr(sz, ':'); if (file) *file++ = 0;
			snprintf(n->name, sizeof(n->name), "%s", a);
			if (bo_new(fd, strtoull(sz, 0, 0), &n->bo) || !bo_map(fd, &n->bo)) { perror("bo"); return 2; }
			memset(n->bo.map, 0, n->bo.size);
			if (file) {
				f = fopen(file, "rb");
				if (!f) { perror(file); return 2; }
				if (!fread(n->bo.map, 1, n->bo.size, f)) fprintf(stderr, "warning: empty %s\n", file);
				fclose(f);
			}
		} else if (!strcmp(argv[i], "--dump") && i + 1 < argc && nd < 8) {
			dumps[nd++] = argv[++i];
		}
	}
	/* pass 2: relocs and raw sets (need buffers to exist) */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--reloc") && i + 1 < argc) {
			unsigned reg; char name[32]; unsigned long long off = 0; struct nbo *n;
			if (sscanf(argv[++i], "%u=%31[^+]+%llu", &reg, name, &off) < 2 || !(n = find(bos, nb, name))) {
				fprintf(stderr, "bad --reloc %s\n", argv[i]); return 2;
			}
			job_reloc(&j, reg, n->bo.handle, VC8K_BO_READ | VC8K_BO_WRITE, off);
		} else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
			unsigned reg; unsigned long v;
			if (sscanf(argv[++i], "%u=%li", &reg, &v) != 2 || reg >= VC8K_NUM_REGS) return 2;
			j.regs[reg] = v;
		}
	}

	if (job_submit(fd, &j, &id)) { perror("submit"); return 1; }
	if (job_wait(fd, id, timeout_ms * 1000000ull, &w, out)) { perror("wait"); return 1; }
	printf("result=%u dec_status=0x%08x pp_status=0x%08x\n", w.result, w.dec_status, w.pp_status);
	for (i = 0; i < VC8K_NUM_REGS; i++) printf("reg[%3d]=%08x%s", i, out[i], i % 4 == 3 ? "\n" : "  ");
	printf("\n");

	for (i = 0; i < nd; i++) {
		char *a = strdup(dumps[i]), *file = strchr(a, ':'), *len; struct nbo *n; size_t l;
		*file++ = 0; len = strchr(file, ':'); if (len) *len++ = 0;
		n = find(bos, nb, a);
		if (!n) { fprintf(stderr, "no bo %s\n", a); return 2; }
		l = len ? strtoull(len, 0, 0) : n->bo.size;
		f = fopen(file, "wb");
		if (!f || fwrite(n->bo.map, 1, l, f) != l) { perror(file); return 1; }
		fclose(f);
		printf("dumped %zu bytes of %s -> %s\n", l, a, file);
	}
	return w.result == VC8K_RESULT_OK && (w.dec_status & VC8K_STATUS_RDY) ? 0 : 1;
}
