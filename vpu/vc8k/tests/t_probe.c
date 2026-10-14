/* P1: node exists, driver identifies itself, capability registers sane. */
#include "vc8k_test.h"

int main(void)
{
	static const struct { uint32_t p; const char *n; } params[] = {
		{ VC8K_PARAM_UAPI_VERSION, "uapi_version" }, { VC8K_PARAM_HW_ID, "hw_id" },
		{ VC8K_PARAM_SYNTH_CFG, "synth_cfg(reg50)" }, { VC8K_PARAM_SYNTH_CFG_2, "synth_cfg_2(reg54)" },
		{ VC8K_PARAM_FUSE_CFG, "fuse_cfg(reg57)" }, { VC8K_PARAM_PP_FUSE_CFG, "pp_fuse(reg99)" },
		{ VC8K_PARAM_PP_SYNTH_CFG, "pp_synth(reg100)" }, { VC8K_PARAM_CLK_RATE_HZ, "clk_rate_hz" },
		{ VC8K_PARAM_DMA_MASK_BITS, "dma_bits" }, { VC8K_PARAM_NUM_REGS, "num_regs" },
	};
	uint64_t v, hw_id = 0;
	struct drm_version ver = { 0 };
	char name[32] = "", desc[64] = "";
	int fd, i;

	TEST_BEGIN("probe");
	fd = vc8k_open();
	if (fd < 0) { printf("cannot open vc8k render node: %s\n", strerror(errno)); return 2; }

	ver.name = name; ver.name_len = sizeof(name) - 1;
	ver.desc = desc; ver.desc_len = sizeof(desc) - 1;
	CHECK(!vc8k_ioc(fd, DRM_IOCTL_VERSION, &ver), "DRM_IOCTL_VERSION");
	printf("  driver %s v%d.%d (%s)\n", name, ver.version_major, ver.version_minor, desc);

	for (i = 0; i < (int)(sizeof(params) / sizeof(params[0])); i++) {
		CHECK(!vc8k_param(fd, params[i].p, &v), "get_param %s", params[i].n);
		printf("  %-22s = 0x%llx (%llu)\n", params[i].n, (unsigned long long)v, (unsigned long long)v);
		if (params[i].p == VC8K_PARAM_HW_ID) hw_id = v;
		if (params[i].p == VC8K_PARAM_UAPI_VERSION) CHECK(v == VC8K_UAPI_VERSION, "uapi version %llu", (unsigned long long)v);
		if (params[i].p == VC8K_PARAM_NUM_REGS) CHECK(v == VC8K_NUM_REGS, "num regs");
		if (params[i].p == VC8K_PARAM_DMA_MASK_BITS) CHECK(v >= 32, "dma bits");
		if (params[i].p == VC8K_PARAM_CLK_RATE_HZ && !v) printf("  WARN: clock rate reads 0\n");
	}
	CHECK(hw_id != 0 && hw_id != 0xffffffffu, "hw_id 0x%llx looks like a dead bus", (unsigned long long)hw_id);
	printf("  product id 0x%04x, build 0x%04x\n", (unsigned)(hw_id >> 16), (unsigned)(hw_id & 0xffff));

	CHECK(vc8k_param(fd, 0xdead, &v) == -1 && errno == EINVAL, "unknown param -> EINVAL");
	close(fd);
	TEST_END("probe");
	return t_summary();
}
