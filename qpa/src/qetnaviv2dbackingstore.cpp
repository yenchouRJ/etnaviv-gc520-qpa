/*
 * qetnaviv2dbackingstore.cpp — Backing store implementation.
 *
 * flush() sequence:
 *  1. Unlock the blittable (drains any pending CPU writes + GPU fence).
 *  2. etna2d_blit(): copy the entire backing BO into the scanout back-buffer.
 *     Only the dirty region needs to be copied; we blit the union of the
 *     dirty rects for simplicity in v1.
 *  3. etna2d_finish(): wait for the GPU blit to complete before flip.
 *  4. screen->pageFlip(): display the back-buffer (blocks for vblank).
 *
 * Reference: src/plugins/platforms/directfb/qdirectfbbackingstore.cpp
 */
#include "qetnaviv2dbackingstore.h"

#include <QDebug>
#include <qpa/qwindowsysteminterface.h>

QT_BEGIN_NAMESPACE

static bool bsDebug()
{
    static int v = qEnvironmentVariableIntValue("QT_ETNAVIV2D_DEBUG");
    return v > 0;
}

#define BS_DBG  if (bsDebug()) qDebug() << "[etnaviv2d|backingstore]"
#define BS_WARN qWarning() << "[etnaviv2d|backingstore]"

/* -----------------------------------------------------------------------
 * Construction / destruction
 * ----------------------------------------------------------------------- */
QEtnaviv2dBackingStore::QEtnaviv2dBackingStore(QWindow *window,
                                               QEtnaviv2dScreen *screen)
    : QPlatformBackingStore(window)
    , m_screen(screen)
{
    BS_DBG << "created for window" << window;
    /* Allocate initial backing surface sized to the window. */
    resize(window->size(), QRegion());
}

QEtnaviv2dBackingStore::~QEtnaviv2dBackingStore()
{
    BS_DBG << "destroyed";
}

/* -----------------------------------------------------------------------
 * paintDevice — the surface Qt paints into
 * ----------------------------------------------------------------------- */
QPaintDevice *QEtnaviv2dBackingStore::paintDevice()
{
    return m_pixmap.data();
}

/* -----------------------------------------------------------------------
 * resize — reallocate backing surface on window geometry changes
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::resize(const QSize &size,
                                    const QRegion & /*staticContents*/)
{
    if (!size.isValid() || size.isEmpty())
        return;

    /* Nothing to do if size unchanged. */
    if (m_pmdata && m_pmdata->width() == size.width() &&
                    m_pmdata->height() == size.height()) {
        return;
    }

    BS_DBG << "resize" << size;

    /* Create a new EtnaBlittablePlatformPixmap; it owns an offscreen BO. */
    auto *pmdata = new EtnaBlittablePlatformPixmap();
    pmdata->resize(size.width(), size.height());

    m_pixmap.reset(new QPixmap(pmdata)); /* QPixmap takes ownership of pmdata */
    m_pmdata = pmdata;

    BS_DBG << "resize done: backing surf=" << (m_pmdata->etnaBlittable()
            ? m_pmdata->etnaBlittable()->surface() : nullptr);
}

/* -----------------------------------------------------------------------
 * flush — blit backing BO → scanout back-buffer, then page-flip
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::flush(QWindow * /*window*/,
                                   const QRegion &region,
                                   const QPoint  &offset)
{
    Q_UNUSED(offset); /* offset is always (0,0) for full-screen QFbScreen */

    if (!m_pmdata || !m_pmdata->etnaBlittable()) {
        BS_WARN << "flush: no backing blittable";
        return;
    }

    EtnaBlittable *blitter    = m_pmdata->etnaBlittable();
    etna2d_context *ctx       = blitter->context();
    etna2d_surface *backingSurf = blitter->surface();
    etna2d_surface *scanoutBack = m_screen->backSurface();

    if (!scanoutBack) {
        /* In wayland stub mode there is no KMS scanout surface — this is
         * expected and silent.  Any other mode with a missing back surface
         * is a real error. */
        if (!m_screen->isStub())
            BS_WARN << "flush: no scanout back surface";
        return;
    }

    /* 1. Unlock: drain CPU writes + GPU fence for any raster-fallback ops. */
    blitter->unlock();

    /* 2. GPU blit the full backing surface into the scanout back-buffer.
     *
     *    We do NOT use the dirty region here.  With double-buffering the back
     *    buffer contains stale content from two frames ago; blitting only the
     *    dirty rect leaves the remainder of the back buffer with that stale
     *    image, causing "ghosting" when widgets move (e.g. moveblocks).
     *
     *    A full-surface blit costs one DE submit (~3µs fixed cost + pixel fill
     *    time) which is identical to the dirty-region path for full-screen
     *    repaints and only marginally more expensive for small dirty regions.
     *    The optimisation can be re-introduced later using a "shadow back"
     *    copy-on-flip scheme.
     */
    int surfW = etna2d_surface_width(backingSurf);
    int surfH = etna2d_surface_height(backingSurf);
    BS_DBG << "flush: full blit" << surfW << "x" << surfH
           << "(dirty=" << region.boundingRect() << "ignored for correctness)";

    etna2d_blit(ctx,
                backingSurf, 0, 0,
                scanoutBack, 0, 0,
                surfW, surfH,
                ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);

    /* 3. Wait for GPU blit to complete before handing the buffer to display. */
    if (etna2d_finish(ctx) != ETNA2D_OK) {
        BS_WARN << "flush: etna2d_finish failed";
        return;
    }

    /* 4. Page-flip (blocks until vblank). */
    if (m_screen->pageFlip() != 0)
        BS_WARN << "flush: pageFlip failed";
}

/* -----------------------------------------------------------------------
 * toImage — for screenshots / accessibility
 * ----------------------------------------------------------------------- */
QImage QEtnaviv2dBackingStore::toImage() const
{
    return m_pixmap ? m_pixmap->toImage() : QImage();
}

/* -----------------------------------------------------------------------
 * scroll — GPU blit within the backing surface (avoids CPU memcpy)
 * ----------------------------------------------------------------------- */
bool QEtnaviv2dBackingStore::scroll(const QRegion &area, int dx, int dy)
{
    if (!m_pmdata || !m_pmdata->etnaBlittable())
        return false;

    /* Unlock first so we don't mix CPU and GPU reads of the same surface. */
    m_pmdata->blittable()->unlock();

    EtnaBlittable *blitter  = m_pmdata->etnaBlittable();
    etna2d_context *ctx     = blitter->context();
    etna2d_surface *surf    = blitter->surface();

    for (const QRect &r : area) {
        if (r.isEmpty()) continue;
        /* Blit-within-surface: src and dst may overlap; safe because the DE
         * reads the full source before writing any dest pixel. */
        etna2d_blit(ctx,
                    surf, r.x(),      r.y(),
                    surf, r.x() + dx, r.y() + dy,
                    r.width(), r.height(),
                    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
    }
    /* Flush scroll ops so they are visible before the next paint. */
    etna2d_flush(ctx);

    BS_DBG << "scroll" << area.boundingRect() << "dx=" << dx << "dy=" << dy;
    return true;
}

QT_END_NAMESPACE
