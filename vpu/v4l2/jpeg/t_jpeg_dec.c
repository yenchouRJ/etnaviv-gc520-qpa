/*
 * t_jpeg_dec — decode JPEG files with the Hantro JPEG decoder (V4L2 mem2mem, no gstreamer).
 *
 *   t_jpeg_dec [-d /dev/videoN] [-f nv12|xrgb|rgb565] [-s WxH] [-n loops] [-o outdir] [-x] img.jpg...
 *
 *   -s WxH   scale to WxH on the CAPTURE side (PP scaler); default: the picture size
 *   -n N     decode each image N times (timing; the last result is written)
 *   -o dir   write dir/<name>.<fmt> (the whole CAPTURE buffer, bytesperline = width) and
 *            dir/<name>.size ("W H CW CH": CAPTURE buffer size, visible rectangle)
 *   -x       the image is expected to be rejected: pass when the job fails cleanly (error flag /
 *            ioctl error), fail when it decodes or hangs
 *
 * The OUTPUT format gets the picture size read from the first SOF marker, like a parser would
 * tell it.  One fd per image.  Exit code 0 when every image behaved as expected.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define TMO_MS 5000

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Picture size of the first SOFn marker; 0 when there is none */
static int jpeg_size(const unsigned char *b, size_t n, unsigned *w, unsigned *h)
{
	size_t i = 2;

	if (n < 4 || b[0] != 0xff || b[1] != 0xd8)
		return -1;
	while (i + 4 <= n) {
		unsigned m, len;

		if (b[i] != 0xff) {
			i++;
			continue;
		}
		m = b[i + 1];
		if (m == 0xff) {
			i++;
			continue;
		}
		if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7) || m == 0) {
			i += 2;
			continue;
		}
		len = b[i + 2] << 8 | b[i + 3];
		if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc && i + 9 <= n) {
			*h = b[i + 5] << 8 | b[i + 6];
			*w = b[i + 7] << 8 | b[i + 8];
			return 0;
		}
		i += 2 + len;
	}
	return -1;
}

struct buf { void *p; size_t len; };

static int xioctl(int fd, unsigned long r, void *a)
{
	int e;

	do { e = ioctl(fd, r, a); } while (e < 0 && errno == EINTR);
	return e;
}

static int wait_dq(int fd, unsigned type, struct v4l2_buffer *vb, struct v4l2_plane *pl)
{
	struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };
	double t0 = now_ms();

	for (;;) {
		memset(vb, 0, sizeof(*vb));
		memset(pl, 0, sizeof(*pl));
		vb->type = type;
		vb->memory = V4L2_MEMORY_MMAP;
		vb->m.planes = pl;
		vb->length = 1;
		if (!xioctl(fd, VIDIOC_DQBUF, vb))
			return 0;
		if (errno != EAGAIN)
			return -errno;
		if (now_ms() - t0 > TMO_MS)
			return -ETIMEDOUT;
		poll(&p, 1, 100);
	}
}

static int decode(const char *dev, const char *path, unsigned fmt, const char *ext, unsigned sw,
		  unsigned sh, int loops, const char *outdir, int expect_fail)
{
	unsigned char *img;
	FILE *f = fopen(path, "rb");
	long n;
	unsigned w = 64, h = 64, cw, ch, i;
	int fd, ret = 1, ok = 0;
	struct v4l2_format fo = {}, fc = {};
	struct v4l2_requestbuffers rb = {};
	struct v4l2_buffer vb = {};
	struct v4l2_plane pl = {};
	struct v4l2_selection sel = {};
	struct buf bo = {}, bc = {};
	unsigned type;
	double t0, t1, best = 1e9, sum = 0;
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char msg[160] = "";
	int err = 0;

	if (!f) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	img = malloc(n ? n : 1);
	if (fread(img, 1, n, f) != (size_t)n) {
		fclose(f);
		return 1;
	}
	fclose(f);
	if (jpeg_size(img, n, &w, &h))
		w = h = 64;		/* no usable header: let the driver reject it */

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	fo.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fo.fmt.pix_mp.width = w;
	fo.fmt.pix_mp.height = h;
	fo.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
	fo.fmt.pix_mp.num_planes = 1;
	fo.fmt.pix_mp.plane_fmt[0].sizeimage = (n + 4095) & ~4095L;
	if (fo.fmt.pix_mp.plane_fmt[0].sizeimage < 65536)
		fo.fmt.pix_mp.plane_fmt[0].sizeimage = 65536;
	if (xioctl(fd, VIDIOC_S_FMT, &fo)) {
		snprintf(msg, sizeof(msg), "S_FMT(OUTPUT) %s", strerror(errno));
		goto out;
	}

	if (fo.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_JPEG) {
		snprintf(msg, sizeof(msg), "driver has no JPEG decoder (OUTPUT format %.4s)", (char *)&fo.fmt.pix_mp.pixelformat);
		goto out;
	}

	fc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fc.fmt.pix_mp.width = sw ? sw : w;
	fc.fmt.pix_mp.height = sh ? sh : h;
	fc.fmt.pix_mp.pixelformat = fmt;
	fc.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &fc)) {
		snprintf(msg, sizeof(msg), "S_FMT(CAPTURE) %s", strerror(errno));
		goto out;
	}
	cw = fc.fmt.pix_mp.width;
	ch = fc.fmt.pix_mp.height;
	if (fc.fmt.pix_mp.pixelformat != fmt) {
		snprintf(msg, sizeof(msg), "CAPTURE format %.4s not accepted", (char *)&fc.fmt.pix_mp.pixelformat);
		goto out;
	}

	/* visible rectangle */
	sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	sel.target = V4L2_SEL_TGT_COMPOSE;
	if (xioctl(fd, VIDIOC_G_SELECTION, &sel))
		sel.r.width = sel.r.height = 0;

	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	rb.count = 1;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb)) {
		snprintf(msg, sizeof(msg), "REQBUFS(OUTPUT) %s", strerror(errno));
		goto out;
	}
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb)) {
		snprintf(msg, sizeof(msg), "REQBUFS(CAPTURE) %s", strerror(errno));
		goto out;
	}

	for (i = 0; i < 2; i++) {
		struct buf *b = i ? &bc : &bo;

		memset(&vb, 0, sizeof(vb));
		memset(&pl, 0, sizeof(pl));
		vb.type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		vb.memory = V4L2_MEMORY_MMAP;
		vb.m.planes = &pl;
		vb.length = 1;
		if (xioctl(fd, VIDIOC_QUERYBUF, &vb)) {
			snprintf(msg, sizeof(msg), "QUERYBUF %s", strerror(errno));
			goto out;
		}
		b->len = pl.length;
		b->p = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl.m.mem_offset);
		if (b->p == MAP_FAILED) {
			snprintf(msg, sizeof(msg), "mmap %s", strerror(errno));
			goto out;
		}
	}
	if ((size_t)n > bo.len) {
		snprintf(msg, sizeof(msg), "image larger than the OUTPUT buffer");
		goto out;
	}
	memcpy(bo.p, img, n);
	memset(bc.p, 0x55, bc.len);

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type)) {
		snprintf(msg, sizeof(msg), "STREAMON(OUTPUT) %s", strerror(errno));
		goto out;
	}
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type)) {
		snprintf(msg, sizeof(msg), "STREAMON(CAPTURE) %s", strerror(errno));
		goto out;
	}

	for (i = 0; i < (unsigned)loops; i++) {
		struct v4l2_buffer q = {};
		struct v4l2_plane qp = {};
		int r;

		t0 = now_ms();
		qp.bytesused = n;
		qp.length = bo.len;
		q.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		q.memory = V4L2_MEMORY_MMAP;
		q.m.planes = &qp;
		q.length = 1;
		q.index = 0;
		if (xioctl(fd, VIDIOC_QBUF, &q)) {
			snprintf(msg, sizeof(msg), "QBUF(OUTPUT) %s", strerror(errno));
			goto out;
		}
		memset(&q, 0, sizeof(q));
		memset(&qp, 0, sizeof(qp));
		q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		q.memory = V4L2_MEMORY_MMAP;
		q.m.planes = &qp;
		q.length = 1;
		if (xioctl(fd, VIDIOC_QBUF, &q)) {
			snprintf(msg, sizeof(msg), "QBUF(CAPTURE) %s", strerror(errno));
			goto out;
		}
		r = wait_dq(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, &vb, &pl);
		if (r) {
			snprintf(msg, sizeof(msg), "no decoded buffer: %s", strerror(-r));
			err = r == -ETIMEDOUT ? 2 : 1;	/* 2 = hang */
			goto out;
		}
		t1 = now_ms();
		{
			unsigned flags = vb.flags;
			struct v4l2_buffer vo;
			struct v4l2_plane po;

			if (wait_dq(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, &vo, &po))
				flags |= V4L2_BUF_FLAG_ERROR;
			flags |= vo.flags & V4L2_BUF_FLAG_ERROR;
			if (flags & V4L2_BUF_FLAG_ERROR) {
				snprintf(msg, sizeof(msg), "decode failed (buffer error flag)");
				err = 1;
				goto out;
			}
		}
		if (t1 - t0 < best)
			best = t1 - t0;
		sum += t1 - t0;
	}
	ok = 1;

	if (outdir && ok && !expect_fail) {
		char p[512];
		FILE *o;

		snprintf(p, sizeof(p), "%s/%.*s.%s", outdir, (int)(strlen(name) - 4), name, ext);
		o = fopen(p, "wb");
		if (o) {
			fwrite(bc.p, 1, pl.bytesused ?: fc.fmt.pix_mp.plane_fmt[0].sizeimage, o);
			fclose(o);
		}
		snprintf(p, sizeof(p), "%s/%.*s.size", outdir, (int)(strlen(name) - 4), name);
		o = fopen(p, "w");
		if (o) {
			fprintf(o, "%u %u %u %u\n", cw, ch, sel.r.width, sel.r.height);
			fclose(o);
		}
	}

out:
	if (bo.p && bo.p != MAP_FAILED)
		munmap(bo.p, bo.len);
	if (bc.p && bc.p != MAP_FAILED)
		munmap(bc.p, bc.len);
	close(fd);
	free(img);
	if (expect_fail) {
		if (ok)
			printf("FAIL %s: decoded but a clean rejection was expected\n", name);
		else if (err == 2)
			printf("FAIL %s: HANG (%s)\n", name, msg);
		else
			printf("PASS %s: rejected (%s)\n", name, msg);
		ret = ok || err == 2;
	} else if (ok) {
		printf("PASS %s: %ux%u -> %ux%u visible %ux%u, %.1f ms (avg %.1f of %d)\n", name, w, h,
		       cw, ch, sel.r.width, sel.r.height, best, sum / loops, loops);
		ret = 0;
	} else {
		printf("FAIL %s: %s\n", name, msg);
		ret = 1;
	}
	fflush(stdout);
	return ret;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0", *outdir = NULL, *ext = "nv12";
	unsigned fmt = V4L2_PIX_FMT_NV12, sw = 0, sh = 0;
	int loops = 1, expect_fail = 0, rc = 0, c;

	while ((c = getopt(argc, argv, "d:f:s:n:o:x")) != -1) {
		switch (c) {
		case 'd': dev = optarg; break;
		case 'f':
			if (!strcmp(optarg, "xrgb")) { fmt = V4L2_PIX_FMT_XRGB32; ext = "xrgb"; }
			else if (!strcmp(optarg, "rgb565")) { fmt = V4L2_PIX_FMT_RGB565; ext = "rgb565"; }
			else if (strcmp(optarg, "nv12")) { fprintf(stderr, "bad -f\n"); return 2; }
			break;
		case 's':
			if (sscanf(optarg, "%ux%u", &sw, &sh) != 2) { fprintf(stderr, "bad -s\n"); return 2; }
			break;
		case 'n': loops = atoi(optarg); break;
		case 'o': outdir = optarg; break;
		case 'x': expect_fail = 1; break;
		default: return 2;
		}
	}
	if (optind >= argc || loops < 1) {
		fprintf(stderr, "usage: %s [-d dev] [-f nv12|xrgb|rgb565] [-s WxH] [-n loops] [-o outdir] [-x] img.jpg...\n", argv[0]);
		return 2;
	}
	for (; optind < argc; optind++)
		rc |= decode(dev, argv[optind], fmt, ext, sw, sh, loops, outdir, expect_fail);
	return rc;
}
