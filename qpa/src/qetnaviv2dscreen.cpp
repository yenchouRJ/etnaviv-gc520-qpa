/*
 * qetnaviv2dscreen.cpp — KMS display screen implementation.
 *
 * KMS operations are serialised by m_flipMutex so pageFlip() is safe to call
 * from a render thread (QT_THREADED_PAINTING=1) as well as the GUI thread.
 *
 * Initialization sequence (atomic path, default):
 *  1. Open card node, set DRM_CLIENT_CAP_ATOMIC + UNIVERSAL_PLANES.
 *  2. Enumerate connector → CRTC → plane (by type: primary or overlay).
 *  3. Resolve all atomic property IDs by name per object.
 *  4. Allocate two dumb BOs → drmModeAddFB2 → PRIME-export → etna2d import.
 *  5. Initial atomic modeset:
 *       - Primary plane: ALLOW_MODESET commit (sets CRTC ACTIVE, MODE_ID,
 *         connector CRTC_ID, plane FB_ID + geometry).  Requires DRM master.
 *       - Overlay plane: plane-only commit (FB_ID + geometry, flags=0).
 *         The CRTC is already active; no master needed.
 *  6. Fill QFbScreen geometry fields.
 *
 * Page-flip sequence:
 *  1. etna2d_finish() already called by the backing store.
 *  2. drmModeAtomicCommit(FB_ID only, NONBLOCK | PAGE_FLIP_EVENT).
 *  3. select() + drmHandleEvent() drain loop until flip_pending clears.
 *  4. Swap front/back indices.
 *
 * Cleanup: destroy the mode blob; free BOs and close the fd.  No teardown
 * commit is issued — the kernel releases all KMS state on fd close.
 */
#include "qetnaviv2dscreen.h"

#include <QDebug>
#include <qpa/qwindowsysteminterface.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#include <cstring>

#include <QMutexLocker>

#include <drm_fourcc.h>

QT_BEGIN_NAMESPACE

/* -----------------------------------------------------------------------
 * Logging
 * ----------------------------------------------------------------------- */
static bool screenDebug()
{
    static int v = qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG");
    return v > 0;
}

#define SCR_DBG  if (screenDebug()) qDebug() << "[etnaviv2d|screen]"
#define SCR_WARN qWarning() << "[etnaviv2d|screen]"
#define SCR_INFO qInfo()    << "[etnaviv2d|screen]"

/* -----------------------------------------------------------------------
 * Vblank / page-flip event handler
 * ----------------------------------------------------------------------- */
static void flipHandler(int /*fd*/, unsigned int /*seq*/,
                        unsigned int /*tvSec*/, unsigned int /*tvUsec*/,
                        void *user)
{
    *static_cast<volatile int *>(user) = 0;
}

/* -----------------------------------------------------------------------
 * propId() — resolve a property ID by name on a DRM object.
 *
 * DRM core assigns property IDs per object, so the same named property has
 * different IDs on different planes.  Never hardcode; always resolve by name.
 * Returns 0 if the object does not expose the property (caller must check).
 * ----------------------------------------------------------------------- */
uint32_t QEtnaviv2dScreen::propId(int fd, uint32_t objId, uint32_t objType,
                                  const char *name)
{
    uint32_t id = 0;
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, objId, objType);
    if (!props)
        return 0;
    for (uint32_t i = 0; i < props->count_props && id == 0; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (strcmp(p->name, name) == 0)
            id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

/* -----------------------------------------------------------------------
 * QEtnaviv2dScreen — construction / destruction
 * ----------------------------------------------------------------------- */
QEtnaviv2dScreen::QEtnaviv2dScreen(const QString &cardPath,
                                   PlaneMode      planeMode,
                                   etna2d_context *etnaCtx,
                                   etna2d_device  *etnaDev,
                                   int             sharedKfd)
    : QFbScreen()
    , m_cardPath(cardPath)
    , m_planeMode(planeMode)
    , m_etnaCtx(etnaCtx)
    , m_etnadev(etnaDev)
    , m_ownsKfd(sharedKfd < 0)
{
    if (sharedKfd >= 0)
        m_kfd = sharedKfd;
    for (auto &fb : m_fb) fb = EtnaKmsFb{};
    memset(&m_modeInfo, 0, sizeof(m_modeInfo));
    m_dmabuf[0] = m_dmabuf[1] = -1;
    m_scanoutSurf[0] = m_scanoutSurf[1] = nullptr;
}

QEtnaviv2dScreen::~QEtnaviv2dScreen()
{
    cleanup();
}

/* -----------------------------------------------------------------------
 * initialize() — open KMS, allocate BOs, modeset
 * ----------------------------------------------------------------------- */
bool QEtnaviv2dScreen::initialize()
{
    /* --- Wayland handoff stub: no DRM master, no modeset.
     *     The QPA's direct-KMS transport is disabled; a future Weston
     *     client path switches only this transport, leaving libetna2d/
     *     EtnaBlittable untouched (D11). --- */
    if (m_planeMode == PlaneMode::Wayland) {
        SCR_INFO << "plane=wayland: handoff stub — no KMS ops";
        mGeometry     = QRect(0, 0, 1920, 1080); /* nominal until Weston provides geometry */
        mDepth        = 32;
        mFormat       = QImage::Format_ARGB32_Premultiplied;
        mPhysicalSize = QSizeF(527, 296);         /* nominal 96 dpi for 1920x1080 */
        return true;
    }

    /* --- 1. Open card0 (skip if a shared fd was provided at construction) --- */
    if (m_ownsKfd) {
        m_kfd = open(m_cardPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
        if (m_kfd < 0) {
            SCR_WARN << "open(" << m_cardPath << "):" << strerror(errno);
            return false;
        }
    }
    drmSetClientCap(m_kfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
#ifndef ETNAVIV2D_LEGACY_KMS
    if (drmSetClientCap(m_kfd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        SCR_WARN << "DRM_CLIENT_CAP_ATOMIC:" << strerror(errno);
        cleanup();
        return false;
    }
#endif

    /* --- 2. Enumerate resources, pick connector and CRTC --- */
    drmModeRes *res = drmModeGetResources(m_kfd);
    if (!res) {
        SCR_WARN << "drmModeGetResources failed";
        cleanup();
        return false;
    }

    drmModeConnector *conn = nullptr;
    for (int i = 0; i < res->count_connectors; ++i) {
        conn = drmModeGetConnector(m_kfd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
            break;
        if (conn) { drmModeFreeConnector(conn); conn = nullptr; }
    }
    if (!conn) {
        SCR_WARN << "no connected connector with modes";
        drmModeFreeResources(res);
        cleanup();
        return false;
    }
    m_connId = conn->connector_id;

    /* Preferred mode, else first (highest) mode. */
    m_modeInfo = conn->modes[0];
    for (int i = 0; i < conn->count_modes; ++i) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            m_modeInfo = conn->modes[i];
            break;
        }
    }

    /* Read physical size while we still hold the connector. */
    int mmW = conn->mmWidth;
    int mmH = conn->mmHeight;

    /* Find a CRTC for this connector. */
    for (int i = 0; i < conn->count_encoders && m_crtcId == 0; ++i) {
        drmModeEncoder *enc = drmModeGetEncoder(m_kfd, conn->encoders[i]);
        if (!enc) continue;
        for (int c = 0; c < res->count_crtcs; ++c) {
            if (enc->possible_crtcs & (1u << c)) {
                m_crtcId    = res->crtcs[c];
                m_crtcIndex = c;
                break;
            }
        }
        drmModeFreeEncoder(enc);
    }
    if (m_crtcId == 0) {
        SCR_WARN << "no usable CRTC for connector" << m_connId;
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        cleanup();
        return false;
    }
#ifdef ETNAVIV2D_LEGACY_KMS
    m_savedCrtc = drmModeGetCrtc(m_kfd, m_crtcId);
#endif
    SCR_INFO << "KMS: conn=" << m_connId << "crtc=" << m_crtcId
             << "mode=" << m_modeInfo.name
             << m_modeInfo.hdisplay << "x" << m_modeInfo.vdisplay
             << "@" << m_modeInfo.vrefresh << "Hz";

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    int w = m_modeInfo.hdisplay;
    int h = m_modeInfo.vdisplay;

#ifndef ETNAVIV2D_LEGACY_KMS
    /* --- 2b. Atomic: pick the plane for our CRTC based on m_planeMode --- */
    if (m_crtcIndex < 0) {
        SCR_WARN << "no CRTC index captured";
        cleanup();
        return false;
    }
    const uint32_t targetType = (m_planeMode == PlaneMode::Overlay)
                                ? DRM_PLANE_TYPE_OVERLAY
                                : DRM_PLANE_TYPE_PRIMARY;
    {
        drmModePlaneRes *pres = drmModeGetPlaneResources(m_kfd);
        if (!pres) {
            SCR_WARN << "drmModeGetPlaneResources failed";
            cleanup();
            return false;
        }
        for (uint32_t i = 0; i < pres->count_planes && m_planeId == 0; ++i) {
            drmModePlane *pl = drmModeGetPlane(m_kfd, pres->planes[i]);
            if (!pl)
                continue;
            if (pl->possible_crtcs & (1u << m_crtcIndex)) {
                uint32_t tid = propId(m_kfd, pl->plane_id,
                                      DRM_MODE_OBJECT_PLANE, "type");
                if (tid) {
                    drmModeObjectProperties *pp = drmModeObjectGetProperties(
                        m_kfd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
                    if (pp) {
                        for (uint32_t k = 0; k < pp->count_props; ++k) {
                            if (pp->props[k] == tid &&
                                pp->prop_values[k] == targetType) {
                                m_planeId = pl->plane_id;
                                break;
                            }
                        }
                        drmModeFreeObjectProperties(pp);
                    }
                }
            }
            drmModeFreePlane(pl);
        }
        drmModeFreePlaneResources(pres);
    }
    if (m_planeId == 0) {
        SCR_WARN << "no" << (targetType == DRM_PLANE_TYPE_OVERLAY ? "OVERLAY" : "PRIMARY")
                 << "plane for CRTC index" << m_crtcIndex;
        cleanup();
        return false;
    }

    /* Resolve property IDs by name (per object; 0 == not exposed). */
    m_planeProps.fb_id   = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "FB_ID");
    m_planeProps.crtc_id = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    m_planeProps.src_x   = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "SRC_X");
    m_planeProps.src_y   = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    m_planeProps.src_w   = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "SRC_W");
    m_planeProps.src_h   = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "SRC_H");
    m_planeProps.crtc_x  = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    m_planeProps.crtc_y  = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    m_planeProps.crtc_w  = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    m_planeProps.crtc_h  = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    m_planeProps.zpos    = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "zpos");
    /* overlay-only custom props — expected 0 on the primary plane */
    m_planeProps.blend_mode        = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "porter-duff-blend-mode");
    m_planeProps.source_alpha_mode = propId(m_kfd, m_planeId, DRM_MODE_OBJECT_PLANE, "source-alpha-mode");

    m_crtcProps.active  = propId(m_kfd, m_crtcId, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    m_crtcProps.mode_id = propId(m_kfd, m_crtcId, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    m_connProps.crtc_id = propId(m_kfd, m_connId, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

    if (!m_planeProps.fb_id || !m_planeProps.crtc_id ||
        !m_crtcProps.active || !m_crtcProps.mode_id || !m_connProps.crtc_id) {
        SCR_WARN << "required atomic properties missing"
                 << "(plane.fb_id" << m_planeProps.fb_id
                 << "plane.crtc_id" << m_planeProps.crtc_id
                 << "crtc.active" << m_crtcProps.active
                 << "crtc.mode_id" << m_crtcProps.mode_id
                 << "conn.crtc_id" << m_connProps.crtc_id << ")";
        cleanup();
        return false;
    }

    if (drmModeCreatePropertyBlob(m_kfd, &m_modeInfo, sizeof(m_modeInfo),
                                  &m_modeBlobId)) {
        SCR_WARN << "drmModeCreatePropertyBlob:" << strerror(errno);
        cleanup();
        return false;
    }

    SCR_INFO << "Atomic: plane=" << m_planeId
             << (m_planeMode == PlaneMode::Overlay ? "(overlay)" : "(primary)")
             << "props resolved; mode blob=" << m_modeBlobId;
#endif // !ETNAVIV2D_LEGACY_KMS

    /* --- 3. Allocate dumb BOs and DRM FBs --- */
    for (int i = 0; i < KMS_NUM_BUFS; ++i) {
        struct drm_mode_create_dumb creq = {};
        creq.width  = w;
        creq.height = h;
        creq.bpp    = 32;
        if (drmIoctl(m_kfd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
            SCR_WARN << "CREATE_DUMB[" << i << "]:" << strerror(errno);
            cleanup();
            return false;
        }
        m_fb[i].handle = creq.handle;
        m_fb[i].stride = creq.pitch;
        m_fb[i].size   = creq.size;
        m_fb[i].width  = w;
        m_fb[i].height = h;

        uint32_t handles[4] = {creq.handle};
        uint32_t pitches[4] = {creq.pitch};
        uint32_t offsets[4] = {};
        uint32_t fmt = (m_planeMode == PlaneMode::Overlay)
                       ? DRM_FORMAT_ARGB8888
                       : DRM_FORMAT_XRGB8888;
        if (drmModeAddFB2(m_kfd, w, h, fmt,
                          handles, pitches, offsets, &m_fb[i].fb_id, 0)) {
            SCR_WARN << "drmModeAddFB2[" << i << "]:" << strerror(errno);
            cleanup();
            return false;
        }
        SCR_DBG << "FB[" << i << "] id=" << m_fb[i].fb_id
                << "handle=" << creq.handle
                << "stride=" << creq.pitch;
    }

    /* --- 4. PRIME export + etnaviv import --- */
    for (int i = 0; i < KMS_NUM_BUFS; ++i) {
        if (drmPrimeHandleToFD(m_kfd, m_fb[i].handle,
                               DRM_CLOEXEC | DRM_RDWR, &m_dmabuf[i])) {
            SCR_WARN << "drmPrimeHandleToFD[" << i << "]:" << strerror(errno);
            cleanup();
            return false;
        }
        int rc = etna2d_surface_from_dmabuf(m_etnadev, m_dmabuf[i],
                                            w, h, m_fb[i].stride,
                                            &m_scanoutSurf[i]);
        if (rc != ETNA2D_OK || !m_scanoutSurf[i]) {
            SCR_WARN << "etna2d_surface_from_dmabuf[" << i << "] rc=" << rc;
            cleanup();
            return false;
        }
        SCR_INFO << "Scanout[" << i << "] imported into etnaviv"
                 << "dmabuf=" << m_dmabuf[i];
    }

    /* --- 5. Initial modeset with the front buffer (fill black first) --- */
    etna2d_fill(m_etnaCtx, m_scanoutSurf[m_frontIdx], 0, 0, w, h, 0xff000000u);
    etna2d_finish(m_etnaCtx);

#ifdef ETNAVIV2D_LEGACY_KMS
    if (drmModeSetCrtc(m_kfd, m_crtcId, m_fb[m_frontIdx].fb_id, 0, 0,
                       &m_connId, 1, &m_modeInfo)) {
        SCR_WARN << "drmModeSetCrtc:" << strerror(errno);
        cleanup();
        return false;
    }
#else
    {
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (!req) {
            SCR_WARN << "drmModeAtomicAlloc (modeset) failed";
            cleanup();
            return false;
        }
        /* ALLOW_MODESET is needed only when we are the one bringing the CRTC
         * up: setting ACTIVE, MODE_ID, and connector CRTC_ID.  This is
         * required when the CRTC is not yet active (first opener, or after a
         * full teardown).  When sharing an fd with an already-active CRTC
         * (e.g. overlay added after primary) we skip these properties and
         * commit with flags=0 — a plane-only commit that does not require
         * master privilege beyond holding the shared fd. */
        uint32_t flags = 0;
        bool crtcAlreadyActive = false;
        {
            drmModeCrtc *crtc = drmModeGetCrtc(m_kfd, m_crtcId);
            if (crtc) {
                crtcAlreadyActive = crtc->mode_valid;
                drmModeFreeCrtc(crtc);
            }
        }
        if (!crtcAlreadyActive) {
            drmModeAtomicAddProperty(req, m_crtcId, m_crtcProps.mode_id, m_modeBlobId);
            drmModeAtomicAddProperty(req, m_crtcId, m_crtcProps.active,  1);
            drmModeAtomicAddProperty(req, m_connId, m_connProps.crtc_id, m_crtcId);
            flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
        }
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.fb_id,   m_fb[m_frontIdx].fb_id);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_id, m_crtcId);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_x,  0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_y,  0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_w,  (uint64_t)w << 16);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.src_h,  (uint64_t)h << 16);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_x, 0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_y, 0);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_w, w);
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.crtc_h, h);
        /* Overlay blend props: set once at modeset, persist for all flips.
         * porter-duff-blend-mode=3 (SRC_OVER), source-alpha-mode=0 (None = per-pixel). */
        if (m_planeMode == PlaneMode::Overlay) {
            if (m_planeProps.blend_mode)
                drmModeAtomicAddProperty(req, m_planeId,
                                         m_planeProps.blend_mode, 3);
            if (m_planeProps.source_alpha_mode)
                drmModeAtomicAddProperty(req, m_planeId,
                                         m_planeProps.source_alpha_mode, 0);
        }
        int rc = drmModeAtomicCommit(m_kfd, req, flags, nullptr);
        drmModeAtomicFree(req);
        if (rc) {
            SCR_WARN << "atomic modeset commit:" << strerror(errno);
            cleanup();
            return false;
        }
    }
#endif
    SCR_INFO << "Initial modeset OK — screen" << w << "x" << h;

    /* --- 6. Fill QFbScreen fields --- */
    mGeometry    = QRect(0, 0, w, h);
    mDepth       = 32;
    mFormat      = QImage::Format_ARGB32_Premultiplied;

    /* Physical size — read from connector above; fall back to 96 dpi. */
    if (mmW <= 0 || mmH <= 0) {
        mmW = w * 25.4 / 96.0;
        mmH = h * 25.4 / 96.0;
    }
    mPhysicalSize = QSizeF(mmW, mmH);

    return true;
}

/* -----------------------------------------------------------------------
 * pageFlip() — flip back buffer to display, block for vblank.
 *
 * Serialised by m_flipMutex so render-thread and GUI-thread flushes cannot
 * race.  Returns immediately with -1 if teardown is already in progress
 * (m_shuttingDown), avoiding an EBUSY from a late Ctrl-C flush.
 * ----------------------------------------------------------------------- */
int QEtnaviv2dScreen::pageFlip()
{
    QMutexLocker lock(&m_flipMutex);

    if (m_shuttingDown)
        return -1;

    m_flipPending = 1;
#ifdef ETNAVIV2D_LEGACY_KMS
    if (drmModePageFlip(m_kfd, m_crtcId, m_fb[m_backIdx].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT,
                        static_cast<void *>(const_cast<int *>(&m_flipPending)))) {
        SCR_WARN << "drmModePageFlip:" << strerror(errno);
        m_flipPending = 0;
        return -1;
    }
#else
    {
        /* Geometry is fixed after the initial modeset; only FB_ID changes
         * per flip.  Non-blocking, event-driven — the drain loop below waits
         * on the same DRM_MODE_PAGE_FLIP_EVENT the legacy path used. */
        drmModeAtomicReq *req = drmModeAtomicAlloc();
        if (!req) {
            SCR_WARN << "drmModeAtomicAlloc (flip) failed";
            m_flipPending = 0;
            return -1;
        }
        drmModeAtomicAddProperty(req, m_planeId, m_planeProps.fb_id,
                                 m_fb[m_backIdx].fb_id);
        int rc = drmModeAtomicCommit(m_kfd, req,
                     DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                     static_cast<void *>(const_cast<int *>(&m_flipPending)));
        drmModeAtomicFree(req);
        if (rc) {
            SCR_WARN << "atomic flip commit:" << strerror(errno);
            m_flipPending = 0;
            return -1;
        }
    }
#endif

    /* Wait for the flip event via select + drmHandleEvent. */
    drmEventContext ev = {};
    ev.version           = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = flipHandler;

    while (m_flipPending) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_kfd, &fds);
        int r = select(m_kfd + 1, &fds, nullptr, nullptr, nullptr);
        if (r < 0) {
            if (errno == EINTR) {
                /* Signal received (e.g. SIGINT → QCoreApplication::quit()).
                 * Stop waiting — the flip may never complete if the app is
                 * shutting down, and blocking here would prevent cleanup. */
                m_flipPending = 0;
                return -1;
            }
            SCR_WARN << "select:" << strerror(errno);
            return -1;
        }
        if (FD_ISSET(m_kfd, &fds))
            drmHandleEvent(m_kfd, &ev);
    }

    /* Swap front/back — but only if we are not shutting down.
     * If cleanup() set m_shuttingDown while we were waiting for vblank the
     * flip has already been presented; skip the swap so cleanup() finds
     * m_frontIdx/m_backIdx in a consistent state. */
    if (!m_shuttingDown)
        std::swap(m_frontIdx, m_backIdx);
    SCR_DBG << "pageFlip done — front=" << m_frontIdx
            << "back=" << m_backIdx;
    return 0;
}

/* -----------------------------------------------------------------------
 * cleanup()
 * ----------------------------------------------------------------------- */
void QEtnaviv2dScreen::cleanup()
{
    {
        /* Signal any concurrent pageFlip() to abort, then wait for it to
         * release the mutex before we tear down KMS state under it. */
        QMutexLocker lock(&m_flipMutex);
        m_shuttingDown = true;
    }

#ifdef ETNAVIV2D_LEGACY_KMS
    /* Restore saved CRTC (legacy). */
    if (m_savedCrtc && m_kfd >= 0) {
        drmModeSetCrtc(m_kfd, m_savedCrtc->crtc_id,
                       m_savedCrtc->buffer_id,
                       m_savedCrtc->x, m_savedCrtc->y,
                       &m_connId, 1, &m_savedCrtc->mode);
        drmModeFreeCrtc(m_savedCrtc);
        m_savedCrtc = nullptr;
    }
#else
    /* Atomic path: no teardown commit needed.  The kernel releases all KMS
     * state (plane FBs, CRTC, connector binding) when the DRM fd is closed.
     * Issuing an explicit disable commit here caused EINVAL because
     * detaching the connector while ACTIVE=1 is an invalid atomic transition,
     * and setting ACTIVE=0 triggers atomic_disable → drm_crtc_vblank_off
     * which would break the next session's vsync.  Just let the fd close. */
    if (m_modeBlobId && m_kfd >= 0) {
        drmModeDestroyPropertyBlob(m_kfd, m_modeBlobId);
        m_modeBlobId = 0;
    }
    if (m_savedCrtc) {
        drmModeFreeCrtc(m_savedCrtc);
        m_savedCrtc = nullptr;
    }
#endif

    for (int i = 0; i < KMS_NUM_BUFS; ++i) {
        if (m_scanoutSurf[i]) {
            etna2d_surface_destroy(m_scanoutSurf[i]);
            m_scanoutSurf[i] = nullptr;
        }
        if (m_dmabuf[i] >= 0) {
            close(m_dmabuf[i]);
            m_dmabuf[i] = -1;
        }
        if (m_fb[i].fb_id && m_kfd >= 0) {
            drmModeRmFB(m_kfd, m_fb[i].fb_id);
            m_fb[i].fb_id = 0;
        }
        if (m_fb[i].handle && m_kfd >= 0) {
            struct drm_mode_destroy_dumb dreq = { .handle = m_fb[i].handle };
            drmIoctl(m_kfd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
            m_fb[i].handle = 0;
        }
    }

    if (m_kfd >= 0 && m_ownsKfd) {
        close(m_kfd);
        m_kfd = -1;
    }
}

QT_END_NAMESPACE
