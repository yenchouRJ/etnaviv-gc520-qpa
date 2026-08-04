/*
 * qetnaviv2dscreen.h — KMS display screen for the etnaviv2d QPA plugin.
 *
 * Owns the DRM/KMS display side (card0, dumb BOs, page-flip) using the
 * Phase-3 kms_display helper.  Reuses QFbScreen for the window-stack
 * and damage-tracking scaffolding; overrides doRedraw() to suppress
 * QFbScreen's software compositing (our backing-store flush() drives
 * all scanout updates directly).
 *
 * Double-buffer state: two dumb BOs on card0 → PRIME-exported → imported
 * into etnaviv as etna2d_surface*.  The backing store renders into an
 * independent offscreen surface then etna2d_blit()s into the back scanout
 * BO before flipping (option B, decided in plan.md).
 *
 * Thread safety: all KMS calls are serialised by m_flipMutex.  pageFlip()
 * may be called from a render thread (QT_THREADED_PAINTING=1); the mutex
 * ensures only one flip is in-flight at a time.  m_shuttingDown is set by
 * cleanup() so that a late flush() arriving during teardown is silently
 * dropped instead of racing with a partially-freed KMS state.
 */
#ifndef QETNAVIV2DSCREEN_H
#define QETNAVIV2DSCREEN_H

#include <QtFbSupport/private/qfbscreen_p.h>
#include <QMutex>
#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include "etna2d.h"
}

QT_BEGIN_NAMESPACE

static constexpr int KMS_NUM_BUFS = 2; /* double buffer */

/* Minimal scanout framebuffer descriptor (mirrors kms_display's kms_fb). */
struct EtnaKmsFb {
    uint32_t fb_id  = 0;
    uint32_t handle = 0;
    uint32_t stride = 0;
    uint64_t size   = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

class QEtnaviv2dScreen : public QFbScreen
{
    Q_OBJECT
public:
    /*
     * cardPath    e.g. "/dev/dri/card0"
     * etnaCtx     shared etna2d context (owned by integration)
     * etnaDev     shared etna2d device  (owned by integration)
     */
    QEtnaviv2dScreen(const QString &cardPath,
                     etna2d_context *etnaCtx,
                     etna2d_device  *etnaDev);
    ~QEtnaviv2dScreen() override;

    /* QPlatformScreen */
    QRect geometry()        const override { return mGeometry; }
    int   depth()           const override { return mDepth; }
    QImage::Format format() const override { return mFormat; }
    QSizeF physicalSize()   const override { return mPhysicalSize; }

    /* QFbScreen — suppress the software compositor; backing store drives flip. */
    QRegion doRedraw() override { return QRegion(); }

    /* Initialize KMS: open card, allocate BOs, modeset. Returns false on error. */
    bool initialize() override;

    /* ---- Display API used by the backing store ---- */

    /* The current back-buffer surface (render target for the backing store). */
    etna2d_surface *backSurface() const { return m_scanoutSurf[m_backIdx]; }

    /* Flip back buffer to display; waits for vblank.
     * Returns 0 on success, -1 on error or if shutting down.
     * Thread-safe: serialised by m_flipMutex. */
    int pageFlip();

    /* Raw DRM fd (for drmHandleEvent poll if needed outside the screen). */
    int drmFd() const { return m_kfd; }

private:
    void cleanup();

    QString         m_cardPath;
    etna2d_context *m_etnaCtx;
    etna2d_device  *m_etnadev;

    /* KMS state (replicated from kms_display internals for Qt integration). */
    int             m_kfd         = -1;
    uint32_t        m_connId      = 0;
    uint32_t        m_crtcId      = 0;
    drmModeCrtc    *m_savedCrtc   = nullptr;

    /* Double-buffer scanout resources. */
    struct EtnaKmsFb m_fb[KMS_NUM_BUFS];
    etna2d_surface *m_scanoutSurf[KMS_NUM_BUFS] = {};
    int             m_dmabuf[KMS_NUM_BUFS]       = {-1, -1};
    int             m_frontIdx   = 0;
    int             m_backIdx    = 1;
    volatile int    m_flipPending = 0;

    QMutex          m_flipMutex;                /* serialises pageFlip() callers */
    volatile bool   m_shuttingDown = false;     /* set in cleanup(); drops late flushes */

    /* Saved mode info for geometry. */
    drmModeModeInfo m_modeInfo   = {};
};

QT_END_NAMESPACE

#endif // QETNAVIV2DSCREEN_H
