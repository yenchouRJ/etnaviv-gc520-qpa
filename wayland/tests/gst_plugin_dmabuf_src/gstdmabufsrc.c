/*
 * gstdmabufsrc.c — GStreamer source plugin: raw NV12 file → memory:DMABuf
 *
 * Reads a packed NV12 raw video file, copies each frame into a DRM dumb BO,
 * and exports it as a DMABuf fd wrapped in a GstBuffer with memory:DMABuf
 * caps feature.  This lets waylandsink receive genuine DMABuf NV12 buffers
 * and promote the surface to a KMS hardware plane via Weston — the same path
 * a real VPU decoder would use.
 *
 * Element name: dmabufsrc
 *
 * Properties:
 *   location  (string)  — path to packed NV12 raw file
 *   width     (int)     — frame width  (default 1024)
 *   height    (int)     — frame height (default 600)
 *   framerate (fraction)— output framerate (default 30/1)
 *   loop      (bool)    — loop file on EOF (default TRUE)
 *   device    (string)  — DRM device node (default /dev/dri/card0)
 *
 * Usage in gst-launch-1.0:
 *   GST_PLUGIN_PATH=/mnt/vivante2d/wayland \
 *   XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1 \
 *   gst-launch-1.0 dmabufsrc location=earth_nv12_1024x600.raw \
 *     width=1024 height=600 ! waylandsink sync=false
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

/* ── ring depth ──────────────────────────────────────────────────── */
#define DMABUF_SRC_RING 3

/* ── GObject boilerplate ─────────────────────────────────────────── */
#define GST_TYPE_DMABUF_SRC (gst_dmabuf_src_get_type())
G_DECLARE_FINAL_TYPE(GstDmaBufSrc, gst_dmabuf_src, GST, DMABUF_SRC, GstPushSrc)

typedef struct {
    uint32_t  handle;
    uint64_t  size;
    uint8_t  *map;
    int       fd;         /* exported DMABuf fd (persistent) */
    gboolean  in_use;
} DmaBufSlot;

struct _GstDmaBufSrc {
    GstPushSrc  parent;

    /* properties */
    gchar      *location;
    gint        width;
    gint        height;
    gint        fps_n, fps_d;
    gboolean    loop;
    gchar      *device;

    /* runtime */
    FILE       *fp;
    int         drm_fd;
    DmaBufSlot  slots[DMABUF_SRC_RING];
    gint        ring_head;
    uint32_t    stride;
    gsize       y_size, uv_size;
    GstClockTime next_pts;
    GstClockTime frame_dur;
    GstAllocator *dmabuf_alloc;
};

G_DEFINE_TYPE(GstDmaBufSrc, gst_dmabuf_src, GST_TYPE_PUSH_SRC)

enum {
    PROP_0,
    PROP_LOCATION,
    PROP_WIDTH,
    PROP_HEIGHT,
    PROP_FRAMERATE,
    PROP_LOOP,
    PROP_DEVICE,
};

/* ── slot helpers ────────────────────────────────────────────────── */
static gboolean slot_alloc(GstDmaBufSrc *self, DmaBufSlot *s)
{
    struct drm_mode_create_dumb cd = {
        .width  = self->width,
        .height = self->height * 3 / 2,
        .bpp    = 8,
    };
    if (drmIoctl(self->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) {
        GST_ERROR_OBJECT(self, "DRM_IOCTL_MODE_CREATE_DUMB failed: %m");
        return FALSE;
    }
    s->handle = cd.handle;
    s->size   = cd.size;
    if (!self->stride) self->stride = cd.pitch;

    struct drm_mode_map_dumb md = { .handle = s->handle };
    if (drmIoctl(self->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) {
        GST_ERROR_OBJECT(self, "DRM_IOCTL_MODE_MAP_DUMB failed: %m");
        return FALSE;
    }
    s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  self->drm_fd, md.offset);
    if (s->map == MAP_FAILED) {
        GST_ERROR_OBJECT(self, "mmap failed: %m");
        return FALSE;
    }
    if (drmPrimeHandleToFD(self->drm_fd, s->handle, O_RDWR | O_CLOEXEC, &s->fd)) {
        GST_ERROR_OBJECT(self, "drmPrimeHandleToFD failed: %m");
        return FALSE;
    }
    s->in_use = FALSE;
    return TRUE;
}

static void slot_free(GstDmaBufSrc *self, DmaBufSlot *s)
{
    if (s->map && s->map != MAP_FAILED) munmap(s->map, s->size);
    if (s->fd >= 0)  close(s->fd);
    if (s->handle) {
        struct drm_mode_destroy_dumb dd = { .handle = s->handle };
        drmIoctl(self->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

static void on_buf_finalize(gpointer ud, GstMiniObject *obj)
{
    (void)obj;
    ((DmaBufSlot *)ud)->in_use = FALSE;
}

/* ── GstBaseSrc / GstPushSrc vfuncs ─────────────────────────────── */
static gboolean gst_dmabuf_src_start(GstBaseSrc *bsrc)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(bsrc);

    if (!self->location) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, ("No location set"), (NULL));
        return FALSE;
    }
    self->fp = fopen(self->location, "rb");
    if (!self->fp) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                          ("Cannot open %s: %m", self->location), (NULL));
        return FALSE;
    }
    self->drm_fd = open(self->device, O_RDWR | O_CLOEXEC);
    if (self->drm_fd < 0) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                          ("Cannot open %s: %m", self->device), (NULL));
        fclose(self->fp); self->fp = NULL;
        return FALSE;
    }

    self->stride = 0;
    for (int i = 0; i < DMABUF_SRC_RING; i++) {
        self->slots[i].fd = -1;
        if (!slot_alloc(self, &self->slots[i])) return FALSE;
    }
    self->y_size  = (gsize)self->stride * self->height;
    self->uv_size = (gsize)self->stride * self->height / 2;
    self->frame_dur = gst_util_uint64_scale_int(GST_SECOND, self->fps_d, self->fps_n);
    self->next_pts  = 0;
    self->ring_head = 0;

    GST_INFO_OBJECT(self, "started: %dx%d stride=%u y=%zu uv=%zu fps=%d/%d",
                    self->width, self->height, self->stride,
                    self->y_size, self->uv_size, self->fps_n, self->fps_d);
    return TRUE;
}

static gboolean gst_dmabuf_src_stop(GstBaseSrc *bsrc)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(bsrc);
    for (int i = 0; i < DMABUF_SRC_RING; i++) slot_free(self, &self->slots[i]);
    if (self->fp)   { fclose(self->fp);       self->fp     = NULL; }
    if (self->drm_fd >= 0) { close(self->drm_fd); self->drm_fd = -1; }
    return TRUE;
}

static GstCaps *gst_dmabuf_src_get_caps(GstBaseSrc *bsrc, GstCaps *filter)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(bsrc);
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format",         G_TYPE_STRING,       "NV12",
        "width",          G_TYPE_INT,          self->width,
        "height",         G_TYPE_INT,          self->height,
        "framerate",      GST_TYPE_FRACTION,   self->fps_n, self->fps_d,
        "interlace-mode", G_TYPE_STRING,       "progressive",
        NULL);
    gst_caps_set_features(caps, 0,
        gst_caps_features_new("memory:DMABuf", NULL));

    if (filter) {
        GstCaps *tmp = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(caps);
        caps = tmp;
    }
    return caps;
}

static GstFlowReturn gst_dmabuf_src_create(GstPushSrc *psrc, GstBuffer **outbuf)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(psrc);

    /* pick free slot (busy-wait with back-pressure, max 200ms) */
    DmaBufSlot *s = NULL;
    for (int tries = 0; tries < 40; tries++) {
        DmaBufSlot *c = &self->slots[self->ring_head % DMABUF_SRC_RING];
        if (!c->in_use) { s = c; break; }
        g_usleep(5000);
    }
    if (!s) {
        GST_WARNING_OBJECT(self, "ring stall — dropping frame");
        /* return an empty flow-ok to avoid stopping the pipeline */
        *outbuf = gst_buffer_new();
        return GST_FLOW_OK;
    }
    self->ring_head = (self->ring_head + 1) % DMABUF_SRC_RING;

    /* read Y plane */
    for (int row = 0; row < self->height; row++) {
        guint8 *dst = s->map + (gsize)row * self->stride;
        gsize   n   = fread(dst, 1, self->width, self->fp);
        if (n < (gsize)self->width) {
            if (self->loop) { rewind(self->fp); fread(dst, 1, self->width, self->fp); }
            else             return GST_FLOW_EOS;
        }
    }
    /* read UV plane */
    guint8 *uv = s->map + self->y_size;
    for (int row = 0; row < self->height / 2; row++) {
        guint8 *dst = uv + (gsize)row * self->stride;
        gsize   n   = fread(dst, 1, self->width, self->fp);
        if (n < (gsize)self->width) {
            if (self->loop) { rewind(self->fp); fread(dst, 1, self->width, self->fp); }
            else             return GST_FLOW_EOS;
        }
    }

    /* wrap in GstBuffer with DMABuf memory */
    GstBuffer *buf    = gst_buffer_new();
    GstMemory *mem    = gst_dmabuf_allocator_alloc(self->dmabuf_alloc,
                                                    dup(s->fd),
                                                    self->y_size + self->uv_size);
    gst_buffer_append_memory(buf, mem);

    GST_BUFFER_PTS(buf)      = self->next_pts;
    GST_BUFFER_DURATION(buf) = self->frame_dur;
    GST_BUFFER_DTS(buf)      = GST_CLOCK_TIME_NONE;
    self->next_pts           += self->frame_dur;

    s->in_use = TRUE;
    gst_mini_object_weak_ref(GST_MINI_OBJECT(buf), on_buf_finalize, s);

    *outbuf = buf;
    return GST_FLOW_OK;
}

/* ── GObject property plumbing ───────────────────────────────────── */
static void gst_dmabuf_src_set_property(GObject *obj, guint id,
                                         const GValue *v, GParamSpec *ps)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(obj);
    switch (id) {
    case PROP_LOCATION:  g_free(self->location); self->location = g_value_dup_string(v); break;
    case PROP_WIDTH:     self->width   = g_value_get_int(v);     break;
    case PROP_HEIGHT:    self->height  = g_value_get_int(v);     break;
    case PROP_FRAMERATE:
        self->fps_n = gst_value_get_fraction_numerator(v);
        self->fps_d = gst_value_get_fraction_denominator(v);
        break;
    case PROP_LOOP:      self->loop    = g_value_get_boolean(v); break;
    case PROP_DEVICE:    g_free(self->device); self->device = g_value_dup_string(v); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, ps);
    }
}

static void gst_dmabuf_src_get_property(GObject *obj, guint id,
                                         GValue *v, GParamSpec *ps)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(obj);
    switch (id) {
    case PROP_LOCATION:  g_value_set_string(v, self->location); break;
    case PROP_WIDTH:     g_value_set_int(v, self->width);       break;
    case PROP_HEIGHT:    g_value_set_int(v, self->height);      break;
    case PROP_FRAMERATE: gst_value_set_fraction(v, self->fps_n, self->fps_d); break;
    case PROP_LOOP:      g_value_set_boolean(v, self->loop);    break;
    case PROP_DEVICE:    g_value_set_string(v, self->device);   break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, ps);
    }
}

static void gst_dmabuf_src_finalize(GObject *obj)
{
    GstDmaBufSrc *self = GST_DMABUF_SRC(obj);
    g_free(self->location);
    g_free(self->device);
    gst_object_unref(self->dmabuf_alloc);
    G_OBJECT_CLASS(gst_dmabuf_src_parent_class)->finalize(obj);
}

/* ── class / instance init ───────────────────────────────────────── */
static void gst_dmabuf_src_class_init(GstDmaBufSrcClass *klass)
{
    GObjectClass    *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS(klass);
    GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS(klass);

    gobject_class->set_property = gst_dmabuf_src_set_property;
    gobject_class->get_property = gst_dmabuf_src_get_property;
    gobject_class->finalize     = gst_dmabuf_src_finalize;

    g_object_class_install_property(gobject_class, PROP_LOCATION,
        g_param_spec_string("location", "Location", "Raw NV12 file path",
                            NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_WIDTH,
        g_param_spec_int("width", "Width", "Frame width",
                         1, G_MAXINT, 1024, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_HEIGHT,
        g_param_spec_int("height", "Height", "Frame height",
                         1, G_MAXINT, 600, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_FRAMERATE,
        gst_param_spec_fraction("framerate", "Framerate", "Output framerate",
                                1, 1, G_MAXINT, 1, 30, 1,
                                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_LOOP,
        g_param_spec_boolean("loop", "Loop", "Loop file on EOF",
                             TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(gobject_class, PROP_DEVICE,
        g_param_spec_string("device", "Device", "DRM device node",
                            "/dev/dri/card0", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_set_static_metadata(element_class,
        "DMABuf NV12 Raw Video Source",
        "Source/Video",
        "Reads a packed NV12 raw file into DRM dumb BOs and exports as memory:DMABuf",
        "drm-2d-gc520");

    GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE("src",
        GST_PAD_SRC, GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            "video/x-raw(memory:DMABuf), "
            "format = (string) NV12, "
            "width  = (int) [ 1, MAX ], "
            "height = (int) [ 1, MAX ], "
            "framerate = (fraction) [ 0/1, MAX ]"
        ));
    gst_element_class_add_static_pad_template(element_class, &src_templ);

    basesrc_class->start    = gst_dmabuf_src_start;
    basesrc_class->stop     = gst_dmabuf_src_stop;
    basesrc_class->get_caps = gst_dmabuf_src_get_caps;
    pushsrc_class->create   = gst_dmabuf_src_create;
}

static void gst_dmabuf_src_init(GstDmaBufSrc *self)
{
    self->location    = NULL;
    self->width       = 1024;
    self->height      = 600;
    self->fps_n       = 30;
    self->fps_d       = 1;
    self->loop        = TRUE;
    self->device      = g_strdup("/dev/dri/card0");
    self->drm_fd      = -1;
    self->fp          = NULL;
    self->ring_head   = 0;
    self->dmabuf_alloc = gst_dmabuf_allocator_new();

    /* live source — no seeking, clock-driven */
    gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
    gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(self), TRUE);
}

/* ── plugin entry point ──────────────────────────────────────────── */
static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "dmabufsrc",
                                GST_RANK_NONE, GST_TYPE_DMABUF_SRC);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    dmabufsrc,
    "DMABuf NV12 raw video source for waylandsink on MA35D1",
    plugin_init,
    "1.0",
    "LGPL",
    "drm-2d-gc520",
    "https://github.com/anomalyco/opencode"
)
