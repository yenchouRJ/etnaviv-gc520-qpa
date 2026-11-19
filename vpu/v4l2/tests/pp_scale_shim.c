/*
 * pp_scale_shim — LD_PRELOAD test aid for the PP scaler (T3), not a product.
 * Stock gst 1.22 v4l2slh264dec only G_FMTs the CAPTURE queue; this turns that G_FMT into an
 * S_FMT of PP_W x PP_H (NV12) first, so the driver programs the scaler.  The real fix is the
 * T4 gst patch.   optional PP_CROP_W/H = OUTPUT crop (e.g. 1920x1080).
 * usage: PP_W=1024 PP_H=600 LD_PRELOAD=./pp_scale_shim.so gst-launch-1.0 ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

int ioctl(int fd, unsigned long req, ...)
{
	static int (*real)(int, unsigned long, ...);
	va_list ap;
	void *arg;

	if (!real)
		real = dlsym(RTLD_NEXT, "ioctl");
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (req == VIDIOC_G_FMT && getenv("PP_W") && getenv("PP_H")) {
		struct v4l2_format *f = arg;

		if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			struct v4l2_format s = *f;

			if (getenv("PP_CROP_W") && getenv("PP_CROP_H")) {
				/* visible size of the stream (the SPS control carries no crop) */
				struct v4l2_selection c = {
					.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
					.target = V4L2_SEL_TGT_CROP,
					.r = { .width = atoi(getenv("PP_CROP_W")),
					       .height = atoi(getenv("PP_CROP_H")) },
				};
				if (real(fd, VIDIOC_S_SELECTION, &c) != 0)
					perror("pp_scale_shim: S_SELECTION");
			}
			if (real(fd, VIDIOC_G_FMT, &s) == 0) {
				s.fmt.pix_mp.width = atoi(getenv("PP_W"));
				s.fmt.pix_mp.height = atoi(getenv("PP_H"));
				s.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
				if (real(fd, VIDIOC_S_FMT, &s) != 0)
					perror("pp_scale_shim: S_FMT");
			}
		}
	}
	return real(fd, req, arg);
}
