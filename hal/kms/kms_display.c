/*
 * kms_display.c — legacy-KMS display helper for MA35D1. See kms_display.h.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"

#include "kms_display.h"

#ifndef DIE
#define LOGE(...) do { fprintf(stderr, "[kms:ERR ] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGI(...) do { fprintf(stderr, "[kms:INFO] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

struct kms_display {
	int              fd;
	drmModeRes      *res;
	drmModeConnector *conn;
	uint32_t         conn_id;
	uint32_t         crtc_id;
	uint32_t         saved_crtc_valid;
	drmModeCrtc     *saved_crtc;   /* to restore on close */
	drmModeModeInfo  mode;
	volatile int     flip_pending;
};

/* Find an encoder+crtc that can drive the connector. */
static int pick_crtc(struct kms_display *d, drmModeConnector *conn)
{
	for (int i = 0; i < conn->count_encoders; i++) {
		drmModeEncoder *enc = drmModeGetEncoder(d->fd, conn->encoders[i]);
		if (!enc)
			continue;
		for (int c = 0; c < d->res->count_crtcs; c++) {
			if (enc->possible_crtcs & (1u << c)) {
				d->crtc_id = d->res->crtcs[c];
				drmModeFreeEncoder(enc);
				return 0;
			}
		}
		drmModeFreeEncoder(enc);
	}
	return -1;
}

int kms_open(const char *path, struct kms_display **out)
{
	struct kms_display *d;
	drmModeConnector *conn = NULL;

	if (!path || !out)
		return -1;

	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;

	d->fd = open(path, O_RDWR | O_CLOEXEC);
	if (d->fd < 0) {
		LOGE("open(%s): %m", path);
		free(d);
		return -1;
	}

	/* dumb buffers + prime are legacy-KMS capabilities; no atomic cap needed. */
	drmSetClientCap(d->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	d->res = drmModeGetResources(d->fd);
	if (!d->res) {
		LOGE("drmModeGetResources: %m (is this a KMS card node?)");
		goto err;
	}

	/* First connected connector wins. */
	for (int i = 0; i < d->res->count_connectors; i++) {
		conn = drmModeGetConnector(d->fd, d->res->connectors[i]);
		if (conn && conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0)
			break;
		if (conn) { drmModeFreeConnector(conn); conn = NULL; }
	}
	if (!conn) {
		LOGE("no connected connector with modes");
		goto err;
	}
	d->conn = conn;
	d->conn_id = conn->connector_id;

	/* Preferred mode, else the first (usually highest). */
	d->mode = conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++) {
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
			d->mode = conn->modes[i];
			break;
		}
	}

	if (pick_crtc(d, conn)) {
		LOGE("no usable crtc for connector %u", d->conn_id);
		goto err;
	}

	d->saved_crtc = drmModeGetCrtc(d->fd, d->crtc_id);
	if (d->saved_crtc)
		d->saved_crtc_valid = 1;

	LOGI("card: conn=%u crtc=%u mode=%s %dx%d@%dHz",
	     d->conn_id, d->crtc_id, d->mode.name,
	     d->mode.hdisplay, d->mode.vdisplay, d->mode.vrefresh);

	*out = d;
	return 0;

err:
	if (d->conn) drmModeFreeConnector(d->conn);
	if (d->res)  drmModeFreeResources(d->res);
	close(d->fd);
	free(d);
	return -1;
}

void kms_close(struct kms_display *d)
{
	if (!d)
		return;
	if (d->saved_crtc_valid && d->saved_crtc) {
		drmModeSetCrtc(d->fd, d->saved_crtc->crtc_id,
			       d->saved_crtc->buffer_id,
			       d->saved_crtc->x, d->saved_crtc->y,
			       &d->conn_id, 1, &d->saved_crtc->mode);
	}
	if (d->saved_crtc) drmModeFreeCrtc(d->saved_crtc);
	if (d->conn) drmModeFreeConnector(d->conn);
	if (d->res)  drmModeFreeResources(d->res);
	if (d->fd >= 0) close(d->fd);
	free(d);
}

int kms_width(const struct kms_display *d)   { return d ? d->mode.hdisplay : 0; }
int kms_height(const struct kms_display *d)  { return d ? d->mode.vdisplay : 0; }
int kms_refresh(const struct kms_display *d) { return d ? d->mode.vrefresh : 0; }
int kms_fd(const struct kms_display *d)      { return d ? d->fd : -1; }

int kms_fb_create(struct kms_display *d, struct kms_fb *fb)
{
	struct drm_mode_create_dumb creq;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	int ret;

	if (!d || !fb)
		return -1;
	memset(fb, 0, sizeof(*fb));
	memset(&creq, 0, sizeof(creq));
	creq.width  = d->mode.hdisplay;
	creq.height = d->mode.vdisplay;
	creq.bpp    = 32;

	ret = drmIoctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret) {
		LOGE("CREATE_DUMB: %m");
		return -1;
	}
	fb->handle = creq.handle;
	fb->stride = creq.pitch;
	fb->size   = creq.size;
	fb->width  = creq.width;
	fb->height = creq.height;

	handles[0] = fb->handle;
	pitches[0] = fb->stride;
	ret = drmModeAddFB2(d->fd, fb->width, fb->height, DRM_FORMAT_XRGB8888,
			    handles, pitches, offsets, &fb->fb_id, 0);
	if (ret) {
		LOGE("drmModeAddFB2: %m");
		struct drm_mode_destroy_dumb dreq = { .handle = fb->handle };
		drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		fb->handle = 0;
		return -1;
	}
	LOGI("fb %u: %ux%u stride=%u size=%llu handle=%u",
	     fb->fb_id, fb->width, fb->height, fb->stride,
	     (unsigned long long)fb->size, fb->handle);
	return 0;
}

void kms_fb_destroy(struct kms_display *d, struct kms_fb *fb)
{
	if (!d || !fb)
		return;
	if (fb->map) {
		munmap(fb->map, fb->size);
		fb->map = NULL;
	}
	if (fb->fb_id) {
		drmModeRmFB(d->fd, fb->fb_id);
		fb->fb_id = 0;
	}
	if (fb->handle) {
		struct drm_mode_destroy_dumb dreq = { .handle = fb->handle };
		drmIoctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		fb->handle = 0;
	}
}

void *kms_fb_map(struct kms_display *d, struct kms_fb *fb)
{
	struct drm_mode_map_dumb mreq;

	if (!d || !fb)
		return NULL;
	if (fb->map)
		return fb->map;

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = fb->handle;
	if (drmIoctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		LOGE("MAP_DUMB: %m");
		return NULL;
	}
	fb->map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       d->fd, mreq.offset);
	if (fb->map == MAP_FAILED) {
		LOGE("mmap fb: %m");
		fb->map = NULL;
		return NULL;
	}
	return fb->map;
}

int kms_fb_export_dmabuf(struct kms_display *d, const struct kms_fb *fb)
{
	int fd = -1;
	if (!d || !fb)
		return -1;
	if (drmPrimeHandleToFD(d->fd, fb->handle, DRM_CLOEXEC | DRM_RDWR, &fd)) {
		LOGE("drmPrimeHandleToFD: %m");
		return -1;
	}
	return fd;
}

int kms_set_crtc(struct kms_display *d, const struct kms_fb *fb)
{
	if (!d || !fb)
		return -1;
	if (drmModeSetCrtc(d->fd, d->crtc_id, fb->fb_id, 0, 0,
			   &d->conn_id, 1, &d->mode)) {
		LOGE("drmModeSetCrtc: %m");
		return -1;
	}
	return 0;
}

static void flip_handler(int fd, unsigned int seq, unsigned int tv_sec,
			 unsigned int tv_usec, void *user)
{
	(void)fd; (void)seq; (void)tv_sec; (void)tv_usec;
	*(volatile int *)user = 0;
}

int kms_page_flip(struct kms_display *d, const struct kms_fb *fb)
{
	if (!d || !fb)
		return -1;
	d->flip_pending = 1;
	if (drmModePageFlip(d->fd, d->crtc_id, fb->fb_id,
			    DRM_MODE_PAGE_FLIP_EVENT, (void *)&d->flip_pending)) {
		LOGE("drmModePageFlip: %m");
		d->flip_pending = 0;
		return -1;
	}
	return 0;
}

int kms_wait_flip(struct kms_display *d)
{
	drmEventContext ev = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = flip_handler,
	};
	if (!d)
		return -1;
	while (d->flip_pending) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(d->fd, &fds);
		int r = select(d->fd + 1, &fds, NULL, NULL, NULL);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			LOGE("select: %m");
			return -1;
		}
		if (FD_ISSET(d->fd, &fds))
			drmHandleEvent(d->fd, &ev);
	}
	return 0;
}
