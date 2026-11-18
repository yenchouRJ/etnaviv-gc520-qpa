/* Minimal helpers for the Hantro PP V4L2 tests (same CHECK style as vpu/vc8k/tests). */
#ifndef PP_TEST_H
#define PP_TEST_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

#ifndef V4L2_PIX_FMT_H264_SLICE
#define V4L2_PIX_FMT_H264_SLICE v4l2_fourcc('S', '2', '6', '4')
#endif

static int t_fail, t_pass;
#define CHECK(cond, ...) do {						\
	if (cond) { t_pass++; }						\
	else { t_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__);	\
	       printf(__VA_ARGS__); printf("\n"); }			\
} while (0)

/* Open the decoder and set the OUTPUT (coded) size; the CAPTURE size follows it. */
static int pp_open(const char *dev, unsigned w, unsigned h)
{
	struct v4l2_format f;
	int fd = open(dev, O_RDWR);

	if (fd < 0)
		return -1;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.plane_fmt[0].sizeimage = 1 << 20;
	if (ioctl(fd, VIDIOC_S_FMT, &f)) {
		close(fd);
		return -1;
	}
	return fd;
}

/* S_FMT NV12 on CAPTURE; returns the ioctl result and the driver's answer in *f. */
static int pp_set_cap(int fd, unsigned w, unsigned h, struct v4l2_format *f)
{
	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f->fmt.pix_mp.width = w;
	f->fmt.pix_mp.height = h;
	f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	return ioctl(fd, VIDIOC_S_FMT, f);
}
#endif
