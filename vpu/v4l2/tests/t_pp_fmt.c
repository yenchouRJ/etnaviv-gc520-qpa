/*
 * t_pp_fmt — PP size rules on the CAPTURE queue (no decode).  usage: t_pp_fmt [/dev/videoN]
 * Expected values follow pp/plan.md section 3 / T3: step 8 x 2, 16..4096, up-scale <= 3x
 * (3x-2 height), mixed up/down clamps the up axis, request == coded size (after 16-alignment)
 * stays native (with the MV tail), scaled / forced-PP sizeimage = w*h*3/2 exactly.
 * Run with ma35d1_force_pp=0.
 */
#include "pp_test.h"

struct tc { unsigned srcw, srch, w, h, ow, oh; int scaled; };

static const struct tc tcs[] = {
	/* 1280x720 source */
	{ 1280,  720, 1280,  720, 1280,  720, 0 },
	{ 1280,  720, 1280,  712, 1280,  720, 0 },	/* visible size of an aligned picture */
	{ 1280,  720, 1024,  600, 1024,  600, 1 },
	{ 1280,  720,  640,  360,  640,  360, 1 },
	{ 1280,  720,  512,  300,  512,  300, 1 },
	{ 1280,  720, 1021,  601, 1024,  602, 1 },	/* rounded to step 8 / 2 */
	{ 1280,  720, 1920,  300, 1280,  300, 1 },	/* mixed: width stays at the source */
	{ 1280,  720,  640, 1000,  640,  720, 1 },	/* mixed: height stays at the source */
	{ 1280,  720, 3000, 2000, 3000, 2000, 1 },	/* up-scale */
	{ 1280,  720, 4000, 4000, 3840, 2158, 1 },	/* 3x and 3x-2 */
	{ 1280,  720,    2,    2,   16,   16, 1 },
	/* 1920x1080 source (coded 1920x1088) */
	{ 1920, 1080, 1920, 1080, 1920, 1088, 0 },
	{ 1920, 1080, 1024,  600, 1024,  600, 1 },
	{ 1920, 1080, 1280,  720, 1280,  720, 1 },
	{ 1920, 1080, 5760, 3240, 4096, 3240, 1 },	/* width capped at 4096 */
	{ 1920, 1080, 8000, 8000, 4096, 3262, 1 },
};

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/video0";
	unsigned i, cur_w = 0, cur_h = 0;
	int fd = -1;

	for (i = 0; i < sizeof(tcs) / sizeof(tcs[0]); i++) {
		const struct tc *t = &tcs[i];
		struct v4l2_format f;
		unsigned sw, sh, want;
		int r;

		if (t->srcw != cur_w || t->srch != cur_h) {
			if (fd >= 0)
				close(fd);
			fd = pp_open(dev, t->srcw, t->srch);
			if (fd < 0) { perror("open/S_FMT out"); return 2; }
			cur_w = t->srcw; cur_h = t->srch;
			{
				struct v4l2_frmsizeenum fs = { .pixel_format = V4L2_PIX_FMT_NV12 };

				sw = (t->srcw + 15) & ~15u; sh = (t->srch + 15) & ~15u;
				CHECK(!ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs), "ENUM_FRAMESIZES failed");
				CHECK(fs.type == V4L2_FRMSIZE_TYPE_STEPWISE, "type %u", fs.type);
				CHECK(fs.stepwise.min_width == 16 && fs.stepwise.min_height == 16 &&
				      fs.stepwise.step_width == 8 && fs.stepwise.step_height == 2,
				      "range min/step %ux%u %u/%u", fs.stepwise.min_width,
				      fs.stepwise.min_height, fs.stepwise.step_width, fs.stepwise.step_height);
				CHECK(fs.stepwise.max_width == (3 * sw < 4096 ? 3 * sw : 4096) &&
				      fs.stepwise.max_height == (3 * sh - 2 < 4096 ? 3 * sh - 2 : 4096),
				      "range max %ux%u for coded %ux%u", fs.stepwise.max_width,
				      fs.stepwise.max_height, sw, sh);
				fs.index = 1;
				CHECK(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) < 0 && errno == EINVAL,
				      "index 1 must be EINVAL");
			}
		}
		r = pp_set_cap(fd, t->w, t->h, &f);
		CHECK(r == 0, "S_FMT %ux%u failed errno %d", t->w, t->h, errno);
		CHECK(f.fmt.pix_mp.width == t->ow && f.fmt.pix_mp.height == t->oh,
		      "src %ux%u req %ux%u -> %ux%u, want %ux%u", t->srcw, t->srch, t->w, t->h,
		      f.fmt.pix_mp.width, f.fmt.pix_mp.height, t->ow, t->oh);
		CHECK(f.fmt.pix_mp.plane_fmt[0].bytesperline == t->ow, "stride %u", f.fmt.pix_mp.plane_fmt[0].bytesperline);
		want = t->ow * t->oh * 3 / 2;
		if (t->scaled)
			CHECK(f.fmt.pix_mp.plane_fmt[0].sizeimage == want,
			      "scaled sizeimage %u want %u", f.fmt.pix_mp.plane_fmt[0].sizeimage, want);
		else
			CHECK(f.fmt.pix_mp.plane_fmt[0].sizeimage > want,
			      "native sizeimage %u must carry the MV tail (> %u)",
			      f.fmt.pix_mp.plane_fmt[0].sizeimage, want);
	}
	printf("t_pp_fmt: %d passed, %d failed\n", t_pass, t_fail);
	return t_fail ? 1 : 0;
}
