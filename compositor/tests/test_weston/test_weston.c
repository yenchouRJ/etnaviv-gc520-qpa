/*
 * test_weston.c — Step 6: Weston linux-dmabuf-v1 plane-promotion gate.
 *
 * Purpose:
 *   Prove that a Wayland client's fullscreen NV12 dmabuf surface is promoted
 *   by Weston's DRM backend to a hardware plane (not GL/pixman-composited).
 *   This is the Phase-3 transport validation and de-risks D11 (the future
 *   QPA Wayland transport).
 *
 * Architecture:
 *   - Pure C Wayland client.  No Qt, no QPA, no DRM master.
 *   - Weston is the sole DRM master; this client connects via WAYLAND_DISPLAY.
 *   - 3-buffer NV12 dumb-BO ring (same discipline as test_composite, D9b):
 *       m_staged  = buffer handed to Weston but not yet on-screen
 *       m_onscreen = buffer last confirmed on-screen (cleared by wl_buffer.release)
 *       pickFree() skips both; always one buffer free.
 *   - Each buffer is exported as a dmabuf fd via drmPrimeHandleToFD and
 *     imported into Weston via zwp_linux_dmabuf_v1 with DRM_FORMAT_MOD_LINEAR.
 *     (DRM_FORMAT_MOD_INVALID is rejected by Weston's fb.c:247 — must be LINEAR.)
 *   - Presents NV12 frames read from a .raw file at --video-fps rate.
 *
 * Surface role: wl_surface + xdg_toplevel (xdg-shell, fullscreen)
 *   A bare wl_surface has no shell role and is never "mapped" by kiosk-shell:
 *   - Weston's kiosk-shell only maps surfaces that have been given a shell role.
 *   - An unmapped surface has no output association → schedule_repaint finds
 *     no outputs → repaint loop goes idle → drm_output_assign_planes is never
 *     called again after the first frame → no hardware plane assignment.
 *   Using xdg_wm_base / xdg_surface / xdg_toplevel (fullscreen) gives the
 *   surface a proper role.  Kiosk-shell maps it, Weston repaints it every
 *   frame, and drm_output_assign_planes promotes it to a hardware overlay plane.
 *
 *   wl_surface is still the underlying primitive for all pixel transport:
 *     wl_surface_attach(buffer) → wl_surface_damage_buffer() → wl_surface_commit()
 *   xdg_surface / xdg_toplevel layer only handles the mapping lifecycle
 *   (configure ↔ ack_configure handshake, fullscreen request, close event).
 *
 * Transport mapping (D11 reference):
 *   test_weston (NV12 dumb BO) → dmabuf → linux-dmabuf-v1 → wl_surface_commit
 *   D11 QPA flush() (ARGB etna2d_surface BO) → dmabuf → linux-dmabuf-v1 → wl_surface_commit
 *   Same transport code; only the BO source and format differ.
 *
 * Pass criteria:
 *   1. /sys/kernel/debug/dri/0/state: plane[38] format=NV12 crtc=crtc-0 (active).
 *      plane[31] carries Weston's compositor XRGB shadow FB at zpos=0;
 *      plane[38] carries our NV12 dmabuf at zpos=1 (overlay).
 *   2. Weston drm-backend log: view assigned to a hardware plane (not renderer).
 *      Run weston with: --logger-scopes=drm-backend
 *   3. Tear-free NV12 video visible on screen at target fps.
 *
 * Build:
 *   (from compositor/) make test_weston
 *
 * Run on target (Weston must already be running):
 *   WAYLAND_DISPLAY=wayland-1 ./test_weston \
 *       --raw-video=./images/earth_nv12_1024x600.raw \
 *       --video-size=1024x600 [--video-fps=30] [--frames=N] [--verbose]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <wayland-client.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */
#define NV12_RING_SIZE  3
#define DRM_CARD_PATH   "/dev/dri/card0"

/* -------------------------------------------------------------------------
 * NV12 buffer — one slot in the 3-buffer ring
 * ------------------------------------------------------------------------- */
typedef struct {
    /* DRM side */
    uint32_t  gem_handle;   /* dumb BO GEM handle */
    uint32_t  drm_fb_id;    /* drmModeAddFB2 handle (for debugfs verification) */
    int       dmabuf_fd;    /* prime fd exported for Wayland import */
    void     *map;          /* CPU mmap for memcpy */
    size_t    size;         /* total BO size in bytes */
    uint32_t  stride;       /* Y-plane stride (= width for 8bpp dumb BO) */

    /* Wayland side */
    struct wl_buffer *wl_buf;   /* zwp_linux_dmabuf_v1 created wl_buffer */
    bool              busy;     /* true while Weston holds the buffer */
    bool              created;  /* wl_buffer successfully created */
} NV12Buf;

/* -------------------------------------------------------------------------
 * Global Wayland state
 * ------------------------------------------------------------------------- */
typedef struct {
    struct wl_display    *display;
    struct wl_registry   *registry;
    struct wl_compositor *compositor;
    struct wl_surface    *surface;
    struct xdg_wm_base   *xdg_wm_base;
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *xdg_toplevel;
    bool                  xdg_configured;
    struct zwp_linux_dmabuf_v1 *dmabuf;

    int                   drm_fd;       /* /dev/dri/card0, not DRM master */
    int                   width;
    int                   height;

    NV12Buf               bufs[NV12_RING_SIZE];

    /* ring tracking (mirrors test_composite's m_staged / m_onScreen) */
    NV12Buf              *staged;       /* handed to Weston, not yet released */
    NV12Buf              *onscreen;     /* last released — cleared on release */

    bool                  dmabuf_supported;
    bool                  running;
    bool                  verbose;
} State;

static State g_state;

/* -------------------------------------------------------------------------
 * Timing helpers
 * ------------------------------------------------------------------------- */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms)
{
    if (ms <= 0)
        return;
    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000,
    };
    nanosleep(&ts, NULL);
}

/* -------------------------------------------------------------------------
 * wl_buffer.release listener — clears onscreen flag so pickFree() can reuse
 * ------------------------------------------------------------------------- */
static void buffer_release(void *data, struct wl_buffer *wl_buf)
{
    NV12Buf *buf = (NV12Buf *)data;
    (void)wl_buf;
    buf->busy = false;
    /* clear onscreen pointer if it still points at this buffer */
    if (g_state.onscreen == buf)
        g_state.onscreen = NULL;
    if (g_state.verbose)
        fprintf(stderr, "[weston] released buf dmabuf_fd=%d\n", buf->dmabuf_fd);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

/* -------------------------------------------------------------------------
 * zwp_linux_buffer_params listeners
 * ------------------------------------------------------------------------- */
static void params_created(void *data,
                            struct zwp_linux_buffer_params_v1 *params,
                            struct wl_buffer *new_buf)
{
    NV12Buf *buf = (NV12Buf *)data;
    buf->wl_buf  = new_buf;
    buf->created = true;
    wl_buffer_add_listener(new_buf, &buffer_listener, buf);
    zwp_linux_buffer_params_v1_destroy(params);
    if (g_state.verbose)
        fprintf(stderr, "[weston] params_created: buf=%p wl_buf=%p\n",
                (void *)buf, (void *)new_buf);
}

static void params_failed(void *data,
                           struct zwp_linux_buffer_params_v1 *params)
{
    NV12Buf *buf = (NV12Buf *)data;
    buf->created = false;
    zwp_linux_buffer_params_v1_destroy(params);
    fprintf(stderr, "[weston] ERROR: zwp_linux_buffer_params.create failed"
                    " (buf=%p). Check modifier/format support.\n",
            (void *)buf);
    g_state.running = false;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = params_created,
    .failed  = params_failed,
};

/* -------------------------------------------------------------------------
 * xdg_wm_base ping/pong
 * ------------------------------------------------------------------------- */
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                              uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}
static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

/* xdg_surface configure — ack then commit so Weston maps the surface */
static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                   uint32_t serial)
{
    State *s = (State *)data;
    xdg_surface_ack_configure(xdg_surface, serial);
    s->xdg_configured = true;
    /* Commit with no buffer just to satisfy the protocol state machine */
    wl_surface_commit(s->surface);
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                    int32_t w, int32_t h,
                                    struct wl_array *states)
{
    (void)data; (void)toplevel; (void)w; (void)h; (void)states;
}
static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    State *s = (State *)data;
    (void)toplevel;
    s->running = false;
}
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close     = xdg_toplevel_close,
};

/* -------------------------------------------------------------------------
 * Registry listeners — discover compositor and linux-dmabuf
 * ------------------------------------------------------------------------- */
static void registry_global(void *data,
                             struct wl_registry *registry,
                             uint32_t name,
                             const char *interface,
                             uint32_t version)
{
    State *s = (State *)data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        s->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface,
                                         (version < 4) ? version : 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        s->xdg_wm_base = wl_registry_bind(registry, name,
                                           &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(s->xdg_wm_base, &xdg_wm_base_listener, s);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        /* version 3 is sufficient: create_params + modifiers */
        s->dmabuf = wl_registry_bind(registry, name,
                                     &zwp_linux_dmabuf_v1_interface,
                                     (version < 3) ? version : 3);
        s->dmabuf_supported = true;
    }
}

static void registry_global_remove(void *data,
                                   struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

/* -------------------------------------------------------------------------
 * Buffer allocation
 *
 * Each NV12 buffer is a single dumb BO of size W × (H*3/2) @ 8bpp.
 * Layout: Y plane at offset 0 (stride=W, H rows),
 *         UV plane at offset W*H (stride=W, H/2 rows interleaved).
 *
 * The BO is exported as a dmabuf prime fd, then imported into Weston via
 * zwp_linux_dmabuf_v1 with two planes pointing at the same fd:
 *   plane 0: Y  — offset=0,    stride=W
 *   plane 1: UV — offset=W*H,  stride=W
 * Modifier: DRM_FORMAT_MOD_LINEAR (mandatory — MOD_INVALID is rejected).
 * ------------------------------------------------------------------------- */
static bool alloc_nv12_buf(State *s, NV12Buf *buf, int W, int H)
{
    /* 1. Create dumb BO */
    struct drm_mode_create_dumb creq = {
        .width  = (uint32_t)W,
        .height = (uint32_t)(H * 3 / 2),
        .bpp    = 8,
    };
    if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
        fprintf(stderr, "[weston] CREATE_DUMB failed: %s\n", strerror(errno));
        return false;
    }
    buf->gem_handle = creq.handle;
    buf->size       = creq.size;
    buf->stride     = (uint32_t)W;

    /* 2. mmap for CPU writes */
    struct drm_mode_map_dumb mreq = { .handle = buf->gem_handle };
    if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
        fprintf(stderr, "[weston] MAP_DUMB failed: %s\n", strerror(errno));
        return false;
    }
    buf->map = mmap(NULL, buf->size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, s->drm_fd, (off_t)mreq.offset);
    if (buf->map == MAP_FAILED) {
        buf->map = NULL;
        fprintf(stderr, "[weston] mmap failed: %s\n", strerror(errno));
        return false;
    }

    /* 3. drmModeAddFB2 — registers the BO with KMS so its ID appears in
     *    /sys/kernel/debug/dri/0/state for the kernel cross-check. */
    uint32_t handles[4] = { buf->gem_handle, buf->gem_handle, 0, 0 };
    uint32_t pitches[4] = { buf->stride,     buf->stride,     0, 0 };
    uint32_t offsets[4] = { 0,               (uint32_t)(W * H), 0, 0 };
    if (drmModeAddFB2(s->drm_fd, W, H, DRM_FORMAT_NV12,
                      handles, pitches, offsets, &buf->drm_fb_id, 0)) {
        fprintf(stderr, "[weston] drmModeAddFB2 failed: %s\n", strerror(errno));
        /* Not fatal for Wayland path; log and continue. */
        buf->drm_fb_id = 0;
    }

    /* 4. Export as dmabuf prime fd */
    if (drmPrimeHandleToFD(s->drm_fd, buf->gem_handle, DRM_CLOEXEC,
                           &buf->dmabuf_fd)) {
        fprintf(stderr, "[weston] drmPrimeHandleToFD failed: %s\n", strerror(errno));
        return false;
    }

    /* 5. Import into Weston via zwp_linux_dmabuf_v1.
     *    Use DRM_FORMAT_MOD_LINEAR (not MOD_INVALID — Weston rejects INVALID,
     *    fb.c:247: if modifier == DRM_FORMAT_MOD_INVALID → return NULL). */
    struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(s->dmabuf);
    if (!params) {
        fprintf(stderr, "[weston] create_params failed\n");
        return false;
    }

    /* NV12: two planes in the same dmabuf fd.
     * plane 0 = Y,  offset=0,    stride=W
     * plane 1 = UV, offset=W*H,  stride=W */
    uint64_t mod = DRM_FORMAT_MOD_LINEAR;
    zwp_linux_buffer_params_v1_add(params, buf->dmabuf_fd,
                                   0 /* plane_idx */,
                                   0 /* offset */,
                                   buf->stride,
                                   (uint32_t)(mod >> 32),
                                   (uint32_t)(mod & 0xffffffffU));
    zwp_linux_buffer_params_v1_add(params, buf->dmabuf_fd,
                                   1 /* plane_idx */,
                                   (uint32_t)(W * H) /* offset */,
                                   buf->stride,
                                   (uint32_t)(mod >> 32),
                                   (uint32_t)(mod & 0xffffffffU));

    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, buf);
    buf->created = false;
    zwp_linux_buffer_params_v1_create(params, W, H,
                                       DRM_FORMAT_NV12,
                                       0 /* flags */);

    /* Roundtrip to receive params.created / params.failed synchronously. */
    wl_display_roundtrip(s->display);

    if (!buf->created || !buf->wl_buf) {
        fprintf(stderr, "[weston] wl_buffer creation failed for NV12 buf\n");
        return false;
    }

    if (s->verbose)
        fprintf(stderr, "[weston] alloc buf=%p gem=%u drm_fb=%u dmabuf_fd=%d\n",
                (void *)buf, buf->gem_handle, buf->drm_fb_id, buf->dmabuf_fd);
    return true;
}

static void free_buf(State *s, NV12Buf *buf)
{
    if (buf->wl_buf) {
        wl_buffer_destroy(buf->wl_buf);
        buf->wl_buf = NULL;
    }
    if (buf->dmabuf_fd >= 0) {
        close(buf->dmabuf_fd);
        buf->dmabuf_fd = -1;
    }
    if (buf->map && buf->map != MAP_FAILED) {
        munmap(buf->map, buf->size);
        buf->map = NULL;
    }
    if (buf->drm_fb_id) {
        drmModeRmFB(s->drm_fd, buf->drm_fb_id);
        buf->drm_fb_id = 0;
    }
    if (buf->gem_handle) {
        struct drm_mode_destroy_dumb dreq = { .handle = buf->gem_handle };
        drmIoctl(s->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        buf->gem_handle = 0;
    }
}

/* -------------------------------------------------------------------------
 * pickFree — same discipline as test_composite:
 *   skip staged (handed to Weston, not yet displayed) and
 *   skip onscreen (last confirmed displayed, pending release).
 *   With 3 buffers at most 2 are consumed → always 1 free.
 * ------------------------------------------------------------------------- */
static NV12Buf *pickFree(State *s)
{
    for (int i = 0; i < NV12_RING_SIZE; i++) {
        NV12Buf *b = &s->bufs[i];
        if (b != s->staged && b != s->onscreen && !b->busy)
            return b;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *raw_video  = NULL;
    int         vid_w      = 1024;
    int         vid_h      = 600;
    int         video_fps  = 30;
    int         max_frames = 0;
    bool        verbose    = false;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--raw-video=", 12) == 0)
            raw_video = argv[i] + 12;
        else if (strncmp(argv[i], "--video-size=", 13) == 0)
            sscanf(argv[i] + 13, "%dx%d", &vid_w, &vid_h);
        else if (strncmp(argv[i], "--video-fps=", 12) == 0)
            video_fps = atoi(argv[i] + 12);
        else if (strncmp(argv[i], "--frames=", 9) == 0)
            max_frames = atoi(argv[i] + 9);
        else if (strcmp(argv[i], "--verbose") == 0)
            verbose = true;
        else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "Usage: test_weston --raw-video=FILE --video-size=WxH\n"
                "                   [--video-fps=N] [--frames=N] [--verbose]\n"
                "\n"
                "  --raw-video=FILE    pre-decoded NV12 raw file (mandatory)\n"
                "  --video-size=WxH    frame dimensions (default 1024x600)\n"
                "  --video-fps=N       playback rate (default 30)\n"
                "  --frames=N          stop after N frames (default: loop forever)\n"
                "  --verbose           verbose logging\n"
                "\n"
                "Weston must be running with kiosk-shell + drm-backend.\n"
                "For plane-promotion evidence run weston with:\n"
                "  --logger-scopes=drm-backend\n"
                "Kernel cross-check:\n"
                "  cat /sys/kernel/debug/dri/0/state\n");
            return 0;
        } else {
            fprintf(stderr, "[weston] unknown arg: %s\n", argv[i]);
        }
    }

    if (!raw_video) {
        fprintf(stderr, "[weston] error: --raw-video=FILE is required\n"
                        "  example: --raw-video=./images/earth_nv12_1024x600.raw\n");
        return 1;
    }
    if (video_fps <= 0) video_fps = 30;

    /* ---- Wayland connection ---- */
    memset(&g_state, 0, sizeof(g_state));
    g_state.width   = vid_w;
    g_state.height  = vid_h;
    g_state.verbose = verbose;
    g_state.running = true;
    for (int i = 0; i < NV12_RING_SIZE; i++)
        g_state.bufs[i].dmabuf_fd = -1;

    g_state.display = wl_display_connect(NULL);
    if (!g_state.display) {
        fprintf(stderr, "[weston] wl_display_connect failed — is WAYLAND_DISPLAY set?\n");
        return 1;
    }

    g_state.registry = wl_display_get_registry(g_state.display);
    wl_registry_add_listener(g_state.registry, &registry_listener, &g_state);
    wl_display_roundtrip(g_state.display);
    wl_display_roundtrip(g_state.display); /* second roundtrip for dmabuf format events */

    if (!g_state.compositor) {
        fprintf(stderr, "[weston] wl_compositor not available\n");
        return 1;
    }
    if (!g_state.dmabuf_supported) {
        fprintf(stderr, "[weston] zwp_linux_dmabuf_v1 not available — "
                        "Weston not built with dmabuf support?\n");
        return 1;
    }

    /* ---- Open DRM card for BO allocation (not as DRM master) ---- */
    g_state.drm_fd = open(DRM_CARD_PATH, O_RDWR | O_CLOEXEC);
    if (g_state.drm_fd < 0) {
        fprintf(stderr, "[weston] open(%s) failed: %s\n",
                DRM_CARD_PATH, strerror(errno));
        return 1;
    }
    /* Allow dumb BO creation without being DRM master */
    drmSetClientCap(g_state.drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    /* ---- Create wl_surface + xdg_toplevel role ---- */
    g_state.surface = wl_compositor_create_surface(g_state.compositor);
    if (!g_state.surface) {
        fprintf(stderr, "[weston] wl_compositor_create_surface failed\n");
        return 1;
    }

    if (!g_state.xdg_wm_base) {
        fprintf(stderr, "[weston] xdg_wm_base not available\n");
        return 1;
    }
    g_state.xdg_surface = xdg_wm_base_get_xdg_surface(g_state.xdg_wm_base,
                                                        g_state.surface);
    xdg_surface_add_listener(g_state.xdg_surface, &xdg_surface_listener, &g_state);
    g_state.xdg_toplevel = xdg_surface_get_toplevel(g_state.xdg_surface);
    xdg_toplevel_add_listener(g_state.xdg_toplevel, &xdg_toplevel_listener, &g_state);
    xdg_toplevel_set_title(g_state.xdg_toplevel, "test_weston_nv12");
    xdg_toplevel_set_fullscreen(g_state.xdg_toplevel, NULL);
    /* Commit to trigger initial configure from compositor */
    wl_surface_commit(g_state.surface);
    wl_display_roundtrip(g_state.display);   /* wait for configure event */
    if (!g_state.xdg_configured) {
        fprintf(stderr, "[weston] xdg_surface configure never arrived\n");
        return 1;
    }
    fprintf(stderr, "[weston] xdg_toplevel configured\n");

    /* ---- Allocate 3-buffer NV12 ring ---- */
    fprintf(stderr, "[weston] allocating %d NV12 buffers (%dx%d)\n",
            NV12_RING_SIZE, vid_w, vid_h);
    for (int i = 0; i < NV12_RING_SIZE; i++) {
        if (!alloc_nv12_buf(&g_state, &g_state.bufs[i], vid_w, vid_h)) {
            fprintf(stderr, "[weston] failed to allocate NV12 buf[%d]\n", i);
            return 1;
        }
        fprintf(stderr, "[weston] buf[%d]: gem=%u drm_fb=%u dmabuf_fd=%d\n",
                i,
                g_state.bufs[i].gem_handle,
                g_state.bufs[i].drm_fb_id,
                g_state.bufs[i].dmabuf_fd);
    }

    /* ---- Open raw video file ---- */
    int video_fd = open(raw_video, O_RDONLY);
    if (video_fd < 0) {
        fprintf(stderr, "[weston] open(%s) failed: %s\n", raw_video, strerror(errno));
        return 1;
    }
    off_t file_size   = lseek(video_fd, 0, SEEK_END);
    lseek(video_fd, 0, SEEK_SET);
    size_t frame_bytes  = (size_t)vid_w * vid_h * 3 / 2;
    int    total_frames = (int)(file_size / (off_t)frame_bytes);
    if (total_frames <= 0) {
        fprintf(stderr, "[weston] file too small for even one %dx%d NV12 frame\n",
                vid_w, vid_h);
        return 1;
    }
    fprintf(stderr, "[weston] raw video: %s  %dx%d  %zu bytes/frame  %d frames  fps=%d\n",
            raw_video, vid_w, vid_h, frame_bytes, total_frames, video_fps);

    const int64_t interval_ms = 1000 / video_fps;
    int64_t       start_ms    = now_ms();
    int           frame       = 0;
    int           loop_count  = 0;

    /* ---- Main playback loop ---- */
    while (g_state.running) {
        /* Dispatch pending Weston events (non-blocking) to collect releases */
        while (wl_display_prepare_read(g_state.display) != 0)
            wl_display_dispatch_pending(g_state.display);
        wl_display_read_events(g_state.display);
        wl_display_dispatch_pending(g_state.display);

        /* Pick a free buffer (not staged, not onscreen, not busy) */
        NV12Buf *buf = pickFree(&g_state);
        if (!buf) {
            /* All buffers held by Weston — give the event loop a chance */
            sleep_ms(1);
            continue;
        }

        /* Loop back to start of file */
        int file_frame = frame % total_frames;
        if (file_frame == 0 && frame > 0) {
            ++loop_count;
            if (verbose)
                fprintf(stderr, "[weston] loop %d\n", loop_count);
        }
        off_t seek_pos = (off_t)file_frame * (off_t)frame_bytes;
        if (lseek(video_fd, seek_pos, SEEK_SET) < 0) {
            fprintf(stderr, "[weston] lseek failed: %s\n", strerror(errno));
            break;
        }
        ssize_t n = read(video_fd, buf->map, frame_bytes);
        if (n != (ssize_t)frame_bytes) {
            fprintf(stderr, "[weston] short read at frame %d\n", frame);
            break;
        }

        /* Advance ring state — same pattern as test_composite:
         *   onscreen ← previous staged (about to be committed by Weston)
         *   staged   ← buf (what we are submitting now)
         * Release callback clears onscreen when Weston is done with it. */
        g_state.onscreen = g_state.staged;
        g_state.staged   = buf;
        buf->busy        = true;

        /* Submit to Weston */
        wl_surface_attach(g_state.surface, buf->wl_buf, 0, 0);
        wl_surface_damage_buffer(g_state.surface, 0, 0, vid_w, vid_h);
        wl_surface_commit(g_state.surface);
        wl_display_flush(g_state.display);

        if (verbose && (frame % (video_fps * 5) == 0))
            fprintf(stderr, "[weston] frame=%d loop=%d\n", frame, loop_count);

        frame++;

        if (max_frames > 0 && frame >= max_frames) {
            fprintf(stderr, "[weston] done: frames=%d loops=%d\n",
                    frame, loop_count);
            break;
        }

        /* Pace to --video-fps */
        int64_t elapsed   = now_ms() - start_ms;
        int64_t target_ms = (int64_t)frame * interval_ms;
        int64_t sleep     = target_ms - elapsed;
        if (sleep > 0 && sleep <= interval_ms)
            sleep_ms(sleep);
    }

    /* ---- Cleanup ---- */
    fprintf(stderr, "[weston] shutting down\n");
    close(video_fd);

    /* One final roundtrip to drain any pending release events */
    wl_display_roundtrip(g_state.display);

    for (int i = 0; i < NV12_RING_SIZE; i++)
        free_buf(&g_state, &g_state.bufs[i]);

    if (g_state.surface)  wl_surface_destroy(g_state.surface);
    if (g_state.dmabuf)   zwp_linux_dmabuf_v1_destroy(g_state.dmabuf);
    if (g_state.compositor) wl_compositor_destroy(g_state.compositor);
    if (g_state.registry) wl_registry_destroy(g_state.registry);
    if (g_state.display)  wl_display_disconnect(g_state.display);
    if (g_state.drm_fd >= 0) close(g_state.drm_fd);

    return 0;
}
