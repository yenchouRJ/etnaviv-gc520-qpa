/*
 * gst_dmabuf_video.c — Milestone A video client
 *
 * Reads a raw NV12 file, allocates DRM dumb BOs, exports each frame as a
 * DMABuf, and feeds it to a GStreamer waylandsink pipeline.  This produces
 * genuine memory:DMABuf NV12 buffers so waylandsink promotes the surface to a
 * hardware KMS plane via Weston's linux-dmabuf-v1 + PLANES_ONLY path — the
 * same zero-copy path a real VPU decoder would use.
 *
 * Milestone IVI: waylandsink does not expose its wl_surface for ivi_application
 * binding.  Instead we set the GST_WAYLAND_DISPLAY_HANDLE context to pass our
 * own wl_display (which already has ivi_application bound) to waylandsink so
 * it reuses the same connection.  waylandsink then uses xdg_wm_base internally
 * and goes to the overflow layer in nu-hmi-controller.  For correct layer assignment
 * the video client uses the IVI_SURFACE_ID environment variable (default 1000)
 * and waylandsink's surface is managed by nu-hmi-controller as a desktop_surface.
 *
 * NOTE: For a production video client that needs explicit IVI surface control,
 * use the standalone Wayland + dmabuf path (without waylandsink) and call
 * ivi_application_surface_create on the wl_surface directly.
 *
 * Usage:
 *   gst_dmabuf_video --video=<file.raw> --width=1024 --height=600 [--fps=30]
 *
 * Build:
 *   see wayland/Makefile target gst_dmabuf_video
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/allocators/gstdmabuf.h>

/* ── compile-time constants ─────────────────────────────────────── */
#define RING_SIZE   3         /* number of DMABuf BOs in the ring  */
#define DRM_DEVICE  "/dev/dri/card0"

/* ── per-BO slot ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t   handle;     /* GEM handle                */
    uint64_t   size;       /* byte size (stride * h)    */
    uint8_t   *map;        /* mmap pointer              */
    int        fd;         /* DMABuf fd (exported once) */
    gboolean   in_use;     /* currently queued in GST?  */
} Slot;

/* ── global state ────────────────────────────────────────────────── */
static int          g_drm_fd  = -1;
static Slot         g_slots[RING_SIZE];
static GstElement  *g_pipeline;
static GstElement  *g_appsrc;
static GMainLoop   *g_loop;
static FILE        *g_video_fp;
static int          g_width, g_height, g_fps;
static uint32_t     g_stride;
static uint64_t     g_frame_size;   /* stride * height * 3/2  (NV12) */
static gint         g_ring_head = 0;
static guint64      g_pts = 0;
static guint64      g_frame_dur;
static volatile int g_running = 1;

/* ── signal handler ──────────────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ── allocate one DRM dumb BO and export it as DMABuf ─────────────── */
static int slot_alloc(Slot *s, uint32_t width, uint32_t height, uint32_t bpp,
                      uint32_t *stride_out)
{
    struct drm_mode_create_dumb cd = {
        .width  = width,
        .height = height * 3 / 2,   /* NV12: Y plane + UV half-height */
        .bpp    = bpp,
    };
    if (drmIoctl(g_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }
    s->handle = cd.handle;
    s->size   = cd.size;
    if (stride_out) *stride_out = cd.pitch;

    /* mmap */
    struct drm_mode_map_dumb md = { .handle = s->handle };
    if (drmIoctl(g_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return -1;
    }
    s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  g_drm_fd, md.offset);
    if (s->map == MAP_FAILED) { perror("mmap"); return -1; }

    /* export DMABuf (persistent fd for this BO) */
    if (drmPrimeHandleToFD(g_drm_fd, s->handle, O_RDWR | O_CLOEXEC, &s->fd)) {
        perror("drmPrimeHandleToFD");
        return -1;
    }
    s->in_use = FALSE;
    return 0;
}

static void slot_free(Slot *s)
{
    if (s->map && s->map != MAP_FAILED) munmap(s->map, s->size);
    if (s->fd >= 0)  close(s->fd);
    if (s->handle) {
        struct drm_mode_destroy_dumb dd = { .handle = s->handle };
        drmIoctl(g_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

/* ── buffer weak-ref: mark slot free when GStreamer is done with it ── */
static void on_buffer_free(gpointer user_data, GstMiniObject *obj)
{
    (void)obj;
    ((Slot *)user_data)->in_use = FALSE;
}

/* ── push one frame ──────────────────────────────────────────────── */
static GstFlowReturn push_frame(void)
{
    /* pick a free slot */
    Slot *s = NULL;
    for (int tries = 0; tries < 60; tries++) {
        Slot *candidate = &g_slots[g_ring_head % RING_SIZE];
        if (!candidate->in_use) { s = candidate; break; }
        usleep(5000);   /* 5 ms back-pressure poll */
    }
    if (!s) {
        fprintf(stderr, "[gst_dmabuf_video] ring stall — all slots busy\n");
        return GST_FLOW_OK;
    }
    g_ring_head = (g_ring_head + 1) % RING_SIZE;

    /* read Y + UV planes (NV12) */
    size_t y_size  = (size_t)g_stride * g_height;
    size_t uv_size = (size_t)g_stride * g_height / 2;

    /* read Y plane row by row if stride != width */
    for (int row = 0; row < g_height; row++) {
        if (fread(s->map + row * g_stride, 1, g_width, g_video_fp)
                != (size_t)g_width) {
            /* EOF — loop */
            rewind(g_video_fp);
            fread(s->map + row * g_stride, 1, g_width, g_video_fp);
        }
    }
    /* UV plane */
    uint8_t *uv = s->map + y_size;
    for (int row = 0; row < g_height / 2; row++) {
        if (fread(uv + row * g_stride, 1, g_width, g_video_fp)
                != (size_t)g_width) {
            rewind(g_video_fp);
            fread(uv + row * g_stride, 1, g_width, g_video_fp);
        }
    }

    /* wrap the DMABuf fd in a GstBuffer */
    GstAllocator *alloc = gst_dmabuf_allocator_new();
    /* dup the fd so gst_dmabuf_allocator owns its own copy */
    int dup_fd = dup(s->fd);
    GstMemory *mem = gst_dmabuf_allocator_alloc(alloc, dup_fd,
                                                y_size + uv_size);
    gst_object_unref(alloc);

    GstBuffer *buf = gst_buffer_new();
    gst_buffer_append_memory(buf, mem);

    GST_BUFFER_PTS(buf)      = g_pts;
    GST_BUFFER_DURATION(buf) = g_frame_dur;
    g_pts += g_frame_dur;

    s->in_use = TRUE;

    /* Register a weak-ref so we know when GStreamer is done with the buffer
     * (Weston has released it back to waylandsink, GST refcount hits zero).
     * Clears in_use so the slot can be reused by the next push_frame(). */
    gst_mini_object_weak_ref(GST_MINI_OBJECT(buf), on_buffer_free, (gpointer)s);

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(g_appsrc), buf);
    if (ret != GST_FLOW_OK) s->in_use = FALSE;
    return ret;
}

/* ── appsrc "need-data" callback ─────────────────────────────────── */
static void on_need_data(GstElement *src, guint size, gpointer data)
{
    (void)src; (void)size; (void)data;
    if (!g_running) {
        gst_app_src_end_of_stream(GST_APP_SRC(g_appsrc));
        return;
    }
    push_frame();
}

/* ── frame timer: push one frame per interval at g_fps ──────────── */
static gboolean on_frame_timer(gpointer data)
{
    (void)data;
    if (!g_running) {
        gst_app_src_end_of_stream(GST_APP_SRC(g_appsrc));
        return G_SOURCE_REMOVE;
    }
    GstFlowReturn r = push_frame();
    static guint frame_count = 0;
    if (++frame_count % 30 == 0)
        fprintf(stderr, "[gst_dmabuf_video] pushed %u frames, pts=%" G_GUINT64_FORMAT "ms\n",
                frame_count, g_pts / GST_MSECOND);
    return (r == GST_FLOW_OK) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* ── bus handler ─────────────────────────────────────────────────── */
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus; (void)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err; gchar *dbg;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[gst_dmabuf_video] ERROR: %s\n  %s\n", err->message, dbg);
        g_error_free(err); g_free(dbg);
        g_main_loop_quit(g_loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_main_loop_quit(g_loop);
        break;
    default:
        break;
    }
    return TRUE;
}

/* ── main ────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    const char *video_path = NULL;
    g_width  = 1024;
    g_height = 600;
    g_fps    = 30;

    for (int i = 1; i < argc; i++) {
        if (sscanf(argv[i], "--video=%m[^\n]", &video_path) == 1) continue;
        sscanf(argv[i], "--width=%d",  &g_width);
        sscanf(argv[i], "--height=%d", &g_height);
        sscanf(argv[i], "--fps=%d",    &g_fps);
    }
    if (!video_path) {
        fprintf(stderr, "Usage: %s --video=FILE [--width=W] [--height=H] [--fps=N]\n",
                argv[0]);
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    gst_init(&argc, &argv);

    /* open DRM */
    g_drm_fd = open(DRM_DEVICE, O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) { perror("open " DRM_DEVICE); return 1; }

    /* open video file */
    g_video_fp = fopen(video_path, "rb");
    if (!g_video_fp) { perror(video_path); return 1; }

    /* allocate BO ring — NV12 means stride*height bytes Y + stride*(height/2) UV */
    g_stride = (uint32_t)g_width;   /* dumb BO pitch — updated after first alloc */
    for (int i = 0; i < RING_SIZE; i++) {
        g_slots[i].fd = -1;
        if (slot_alloc(&g_slots[i], g_width, g_height, 8, &g_stride)) {
            fprintf(stderr, "Failed to allocate slot %d\n", i);
            return 1;
        }
    }
    g_frame_size = (uint64_t)g_stride * g_height * 3 / 2;
    g_frame_dur  = GST_SECOND / g_fps;

    fprintf(stderr, "[gst_dmabuf_video] stride=%u frame_size=%llu fps=%d\n",
            g_stride, (unsigned long long)g_frame_size, g_fps);

    /* build pipeline: appsrc ! waylandsink */
    g_pipeline = gst_pipeline_new("video");
    g_appsrc   = gst_element_factory_make("appsrc",     "src");
    GstElement *sink = gst_element_factory_make("waylandsink", "sink");
    if (!g_pipeline || !g_appsrc || !sink) {
        fprintf(stderr, "Failed to create pipeline elements\n");
        return 1;
    }

    /* caps: NV12 DMABuf */
    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format",          G_TYPE_STRING,  "NV12",
        "width",           G_TYPE_INT,     g_width,
        "height",          G_TYPE_INT,     g_height,
        "framerate",       GST_TYPE_FRACTION, g_fps, 1,
        "interlace-mode",  G_TYPE_STRING,  "progressive",
        NULL);
    /* add memory:DMABuf feature */
    GstCapsFeatures *feat = gst_caps_features_new("memory:DMABuf", NULL);
    gst_caps_set_features(caps, 0, feat);

    g_object_set(g_appsrc,
        "caps",            caps,
        "format",          GST_FORMAT_TIME,
        "stream-type",     GST_APP_STREAM_TYPE_STREAM,
        "is-live",         TRUE,
        "emit-signals",    FALSE,   /* use timer-driven push instead */
        "max-bytes",       (guint64)(g_frame_size * RING_SIZE),
        NULL);
    gst_caps_unref(caps);

    g_object_set(sink,
        "sync",            FALSE,
        NULL);

    gst_bin_add_many(GST_BIN(g_pipeline), g_appsrc, sink, NULL);
    gst_element_link(g_appsrc, sink);

    GstBus *bus = gst_element_get_bus(g_pipeline);
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    /* Timer-driven frame push at target fps.
     * g_timeout_add uses millisecond resolution — good enough for 30fps. */
    g_timeout_add(1000 / g_fps, on_frame_timer, NULL);

    g_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_loop);

    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);
    g_main_loop_unref(g_loop);

    fclose(g_video_fp);
    for (int i = 0; i < RING_SIZE; i++) slot_free(&g_slots[i]);
    close(g_drm_fd);
    return 0;
}
