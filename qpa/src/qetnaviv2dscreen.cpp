/*
 * qetnaviv2dscreen.cpp — KMS display screen implementation.
 *
 * Uses the Phase-3 kms_display helper directly (tested and hardware-verified)
 * rather than Qt's eglfs-oriented QKmsDevice.  KMS operations are serialised
 * by m_flipMutex so pageFlip() is safe to call from a render thread
 * (QT_THREADED_PAINTING=1) as well as the GUI thread.
 *
 * Initialization sequence:
 *  1. Open /dev/dri/card0, pick connector + CRTC, query preferred mode.
 *  2. Allocate two dumb BOs via DRM_IOCTL_MODE_CREATE_DUMB.
 *  3. drmModeAddFB2 each BO as a KMS framebuffer.
 *  4. PRIME-export each BO → dma-buf fd.
 *  5. etna2d_surface_from_dmabuf(): import into etnaviv (GPU MMU maps it).
 *  6. Initial drmModeSetCrtc (modeset + first frame blank).
 *  7. Fill QFbScreen geometry fields so Qt knows the screen size.
 *
 * Page-flip sequence (called by backing store flush):
 *  1. etna2d_finish() was already called by the backing store.
 *  2. drmModePageFlip(back fb_id) — non-blocking, event-based.
 *  3. drmHandleEvent() poll loop until flip_pending clears.
 *  4. Swap front/back indices.
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
 * QEtnaviv2dScreen — construction / destruction
 * ----------------------------------------------------------------------- */
QEtnaviv2dScreen::QEtnaviv2dScreen(const QString &cardPath,
                                   etna2d_context *etnaCtx,
                                   etna2d_device  *etnaDev)
    : QFbScreen()
    , m_cardPath(cardPath)
    , m_etnaCtx(etnaCtx)
    , m_etnadev(etnaDev)
{
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
    /* --- 1. Open card0 --- */
    m_kfd = open(m_cardPath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (m_kfd < 0) {
        SCR_WARN << "open(" << m_cardPath << "):" << strerror(errno);
        return false;
    }
    drmSetClientCap(m_kfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

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

    /* Find a CRTC for this connector. */
    for (int i = 0; i < conn->count_encoders && m_crtcId == 0; ++i) {
        drmModeEncoder *enc = drmModeGetEncoder(m_kfd, conn->encoders[i]);
        if (!enc) continue;
        for (int c = 0; c < res->count_crtcs; ++c) {
            if (enc->possible_crtcs & (1u << c)) {
                m_crtcId = res->crtcs[c];
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
    m_savedCrtc = drmModeGetCrtc(m_kfd, m_crtcId);

    SCR_INFO << "KMS: conn=" << m_connId << "crtc=" << m_crtcId
             << "mode=" << m_modeInfo.name
             << m_modeInfo.hdisplay << "x" << m_modeInfo.vdisplay
             << "@" << m_modeInfo.vrefresh << "Hz";

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    int w = m_modeInfo.hdisplay;
    int h = m_modeInfo.vdisplay;

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
        if (drmModeAddFB2(m_kfd, w, h, DRM_FORMAT_XRGB8888,
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

    if (drmModeSetCrtc(m_kfd, m_crtcId, m_fb[m_frontIdx].fb_id, 0, 0,
                       &m_connId, 1, &m_modeInfo)) {
        SCR_WARN << "drmModeSetCrtc:" << strerror(errno);
        cleanup();
        return false;
    }
    SCR_INFO << "Initial modeset OK — screen" << w << "x" << h;

    /* --- 6. Fill QFbScreen fields --- */
    mGeometry    = QRect(0, 0, w, h);
    mDepth       = 32;
    mFormat      = QImage::Format_ARGB32_Premultiplied;

    /* Physical size: derive from DRM connector mm values if available,
     * else assume 96 dpi. */
    int mmW = 0, mmH = 0;
    {
        /* Re-open connector briefly to read mm size. */
        drmModeRes *r2 = drmModeGetResources(m_kfd);
        if (r2) {
            for (int i = 0; i < r2->count_connectors; ++i) {
                drmModeConnector *c2 = drmModeGetConnector(m_kfd, r2->connectors[i]);
                if (c2 && c2->connector_id == m_connId) {
                    mmW = c2->mmWidth;
                    mmH = c2->mmHeight;
                    drmModeFreeConnector(c2);
                    break;
                }
                if (c2) drmModeFreeConnector(c2);
            }
            drmModeFreeResources(r2);
        }
    }
    if (mmW <= 0 || mmH <= 0) {
        /* fallback: 96 dpi */
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
    if (drmModePageFlip(m_kfd, m_crtcId, m_fb[m_backIdx].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT,
                        static_cast<void *>(const_cast<int *>(&m_flipPending)))) {
        SCR_WARN << "drmModePageFlip:" << strerror(errno);
        m_flipPending = 0;
        return -1;
    }

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
     * m_frontIdx/m_backIdx in a consistent state for drmModeSetCrtc. */
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

    /* Restore saved CRTC. */
    if (m_savedCrtc && m_kfd >= 0) {
        drmModeSetCrtc(m_kfd, m_savedCrtc->crtc_id,
                       m_savedCrtc->buffer_id,
                       m_savedCrtc->x, m_savedCrtc->y,
                       &m_connId, 1, &m_savedCrtc->mode);
        drmModeFreeCrtc(m_savedCrtc);
        m_savedCrtc = nullptr;
    }

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

    if (m_kfd >= 0) {
        close(m_kfd);
        m_kfd = -1;
    }
}

QT_END_NAMESPACE
