/*
 * qetnaviv2dbackingstore.cpp — Backing store implementation.
 *
 * flush() sequence (KMS modes):
 *  1. Unlock the blittable (drains any pending CPU writes + GPU fence).
 *  2. etna2d_blit(): copy the entire backing BO into the scanout back-buffer.
 *  3. etna2d_finish(): wait for the GPU blit to complete before flip.
 *  4. screen->pageFlip(): display the back-buffer (blocks for vblank).
 *
 * flush() sequence (Wayland mode):
 *  1. Unlock the blittable.
 *  2. Get the current free ring BO from the transport.
 *  3. etna2d_blit(): copy the backing BO into the ring BO.
 *  4. etna2d_finish(): wait for the GPU blit.
 *  5. transport->commitFrame(): mark dirty; actual wl_surface_attach+commit
 *     happens in the next wl_surface.frame callback (vsync-aligned).
 *
 * Reference: src/plugins/platforms/directfb/qdirectfbbackingstore.cpp
 */
#include "qetnaviv2dbackingstore.h"
#include "qetnaviv2dwaylandtransport.h"
#include "qetnaviv2dstats.h"

#include <QDebug>
#include <QTimer>
#include <QMutexLocker>
#include "qetnaviv2dblitter.h"
#include <string.h>
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
                                               QEtnaviv2dScreen *screen,
                                               QEtnaviv2dWaylandTransport *transport)
    : QPlatformBackingStore(window)
    , m_screen(screen)
    , m_transport(transport)
{
    m_fullFlush = qEnvironmentVariableIntValue("QT_ETNA2D_FULL_FLUSH") > 0;
    if (m_transport) {
        m_transport->setReleaseHook([this]() { presentPending(); });
    } else if (m_screen && m_screen->asyncFlip()) {
        m_screen->armFlipNotifier();
        m_screen->setFlipDoneHook([this]() { presentPending(); });   /* Step 8 */
    }
    BS_DBG << "created for window" << window
           << (transport ? "(wayland mode)" : "(KMS mode)");
    /* Allocate initial backing surface sized to the window. */
    resize(window->size(), QRegion());
}

QEtnaviv2dBackingStore::~QEtnaviv2dBackingStore()
{
    if (m_transport)
        m_transport->setReleaseHook(nullptr);
    else if (m_screen)
        m_screen->setFlipDoneHook(nullptr);
    if (EtnaStats::enabled())
        fprintf(stderr, "ETNAFINAL pending_drop=%d (must be 0: a dropped frame was never presented otherwise)\n", int(m_dropped));
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
    m_stale.clear();   /* every target buffer is stale after a resize */

    /* Create a new EtnaBlittablePlatformPixmap; it owns an offscreen BO. */
    auto *pmdata = new EtnaBlittablePlatformPixmap();
    pmdata->resize(size.width(), size.height());

    m_pixmap.reset(new QPixmap(pmdata)); /* QPixmap takes ownership of pmdata */
    m_pmdata = pmdata;

    BS_DBG << "resize done: backing surf=" << (m_pmdata->etnaBlittable()
            ? m_pmdata->etnaBlittable()->surface() : nullptr);
}

/* QT_ETNA2D_VERIFY_FLUSH=1 (debug): after the blit, the target buffer must equal the
 * backing store everywhere, not only inside the damage region.  Slow (reads WC memory). */
static void verifyTarget(etna2d_surface *src, etna2d_surface *dst, const char *what)
{
    static int en = qEnvironmentVariableIntValue("QT_ETNA2D_VERIFY_FLUSH");
    if (!en || etna2d_surface_format(src) != etna2d_surface_format(dst))
        return;
    const uchar *a = static_cast<const uchar *>(etna2d_surface_map(src));
    const uchar *b = static_cast<const uchar *>(etna2d_surface_map(dst));
    if (!a || !b) return;
    const int w = qMin(etna2d_surface_width(src), etna2d_surface_width(dst));
    const int h = qMin(etna2d_surface_height(src), etna2d_surface_height(dst));
    const int bpp = int(etna2d_surface_stride(src)) / etna2d_surface_width(src);
    static long long frames = 0, bad = 0;
    ++frames;
    for (int y = 0; y < h; ++y) {
        const uchar *ra = a + size_t(y) * etna2d_surface_stride(src);
        const uchar *rb = b + size_t(y) * etna2d_surface_stride(dst);
        if (memcmp(ra, rb, size_t(w) * bpp)) {
            int x = 0;
            while (!memcmp(ra + x * bpp, rb + x * bpp, bpp)) ++x;
            ++bad;
            BS_WARN << "VERIFY FAIL" << what << "frame" << frames << "first diff at x=" << x << "y=" << y
                    << "(bad frames so far" << bad << ")";
            return;
        }
    }
    if (frames % 100 == 0)
        fprintf(stderr, "VERIFYFLUSH frames=%lld bad=%lld\n", frames, bad);
}

/* -----------------------------------------------------------------------
 * Damage tracking helpers
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::accumulateStale(const QRegion &flushed, const QRect &bounds)
{
    const QRegion clipped = m_fullFlush ? QRegion(bounds) : (flushed & bounds);
    for (auto it = m_stale.begin(); it != m_stale.end(); ++it)
        it.value() += clipped;
}

QRegion QEtnaviv2dBackingStore::takeDamage(etna2d_surface *target,
                                           const QRegion &flushed,
                                           const QRect &bounds)
{
    accumulateStale(flushed, bounds);
    auto it = m_stale.find(target);
    if (it == m_stale.end())
        it = m_stale.insert(target, QRegion(bounds));   /* first use: fully stale */
    const QRegion out = it.value() & bounds;
    it.value() = QRegion();
    return out;
}

void QEtnaviv2dBackingStore::blitRegion(etna2d_context *ctx, etna2d_surface *src,
                                        etna2d_surface *dst, const QRegion &r)
{
    /* Cost model (measured Step 0: full 1024x600 blit 8.7 ms = ~14 ns/px; per-op overhead
     * ~30 us = ~2100 px).  One bounding blit beats n rects when the pixels it adds cost less
     * than the (n-1) ops it saves. */
    QVector<QRect> rects = r.rects();
    if (rects.size() > 1) {
        qint64 sum = 0;
        for (const QRect &q : qAsConst(rects)) sum += qint64(q.width()) * q.height();
        const QRect bb = r.boundingRect();
        const qint64 extra = qint64(bb.width()) * bb.height() - sum;
        if (extra < qint64(rects.size() - 1) * 2100 || rects.size() > 128)
            rects = { bb };
    }
    const int bpp = etna2d_surface_stride(src) / qMax(1, etna2d_surface_width(src));
    for (const QRect &q : qAsConst(rects)) {
        if (q.isEmpty()) continue;
        etna2d_blit(ctx, src, q.x(), q.y(), dst, q.x(), q.y(), q.width(), q.height(),
                    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
        EtnaStats::add(EtnaStats::BytesCopied, 0, (long long)q.width() * q.height() * bpp);
    }
}

/* -----------------------------------------------------------------------
 * flush — blit backing BO → target back-buffer, then page-flip or commit
 * ----------------------------------------------------------------------- */
void QEtnaviv2dBackingStore::flush(QWindow * /*window*/,
                                   const QRegion &region,
                                   const QPoint  &offset)
{
    QMutexLocker _etnaLock(&etnaCtxMutex());   /* shared GC520 queue (Step 6) */
    Q_UNUSED(offset); /* offset is always (0,0) for full-screen QFbScreen */

    if (!m_pmdata || !m_pmdata->etnaBlittable()) {
        BS_WARN << "flush: no backing blittable";
        return;
    }

    EtnaBlittable *blitter      = m_pmdata->etnaBlittable();
    etna2d_context *ctx         = blitter->context();
    etna2d_surface *backingSurf = blitter->surface();
    struct FrameEnd { ~FrameEnd() { if (EtnaStats::enabled()) EtnaStats::report(); } } _fe;
    EtnaStats::add(EtnaStats::Frames);

    /* ---- Wayland mode: blit into the ring BO, hand off to transport ---- */
    if (m_transport) {
        /* QT_ETNA2D_TEST_DROP=N (debug): behave as if the ring were full on every Nth flush,
         * to exercise the damage accumulation of dropped frames. */
        /* Step 7: the ring follows the top-level window size (resize(), showFullScreen(), output
         * mode change) unless QT_IVI_GEOMETRY pins it.  Done here, at present time, so a window
         * that is resized several times before painting costs one reallocation. */
        {
            static const bool pinned = !qEnvironmentVariableIsEmpty("QT_IVI_GEOMETRY");
            const QSize want(m_pmdata->width(), m_pmdata->height());
            if (!pinned && window() && window()->type() == Qt::Window
                    && !want.isEmpty() && m_transport->ringSize() != want) {
                m_stale.clear();   /* new ring surfaces: every target is fully stale */
                if (!m_transport->resizeRing(want)) {
                    BS_WARN << "flush(wayland): ring resize to" << want << "failed";
                    return;
                }
            }
        }
        static const int testDrop = qEnvironmentVariableIntValue("QT_ETNA2D_TEST_DROP");
        static int flushCount = 0;
        etna2d_surface *ringBo = (testDrop > 0 && ++flushCount % testDrop == 0) ? nullptr
                                                                           : m_transport->currentBo();
        if (!ringBo) {
            /* All ring slots are in use — Weston hasn't released any yet.
             * Drop the frame (back-pressure): Qt will repaint on the next
             * frame callback when a slot is released. */
            EtnaStats::add(EtnaStats::DroppedFrames);
            accumulateStale(region, QRect(0, 0, m_pmdata->width(), m_pmdata->height()));
            /* Do not lose the frame: the backing store already holds it, only the ring
             * slot is missing.  Present it when Weston releases a slot (wl_buffer.release);
             * if no slot is held (only in the QT_ETNA2D_TEST_DROP test) retry from the
             * event loop. */
            m_dropped = true;
            if (!m_transport->ringBusy() && !m_retryQueued) {
                m_retryQueued = true;
                QTimer::singleShot(0, [this]() { m_retryQueued = false; presentPending(); });
            }
            BS_DBG << "flush: wayland ring full — dropping frame (back-pressure)";
            return;
        }

        /* 1. Unlock backing surface */
        blitter->unlock();

        /* 2. GPU blit only what this ring slot is missing (damage tracking) */
        const QRect bounds(0, 0, etna2d_surface_width(backingSurf), etna2d_surface_height(backingSurf));
        const QRegion damage = takeDamage(ringBo, region, bounds);
        BS_DBG << "flush(wayland): blit" << damage.boundingRect() << "(" << damage.rectCount()
               << "rects) -> ring BO" << ringBo;
        {
            EtnaStats::Scope _t(EtnaStats::FlushBlit);
            blitRegion(ctx, backingSurf, ringBo, damage);
        }

        /* 3. Wait for GPU blit */
        {
            EtnaStats::Scope _tf(EtnaStats::FlushFinish);
            if (etna2d_finish(ctx) != ETNA2D_OK) {
                BS_WARN << "flush(wayland): etna2d_finish failed";
                return;
            }
        }

        verifyTarget(backingSurf, ringBo, "wayland");

        /* 4. Hand off to transport — frame-callback will commit */
        m_dropped = false;
        m_transport->commitFrame(ringBo, damage);
        return;
    }

    /* ---- KMS modes: existing path ---- */
    etna2d_surface *scanoutBack = m_screen->backSurface();

    if (!scanoutBack) {
        /* isStub() with no transport — shouldn't happen after Milestone B,
         * but guard it just in case. */
        if (!m_screen->isStub())
            BS_WARN << "flush: no scanout back surface";
        return;
    }

    /* Fixed screen (Step 8): the scanout is the connector mode; a larger window is clipped to
     * it, a smaller one leaves the (black-cleared) rest untouched. */
    const QRect bounds = QRect(0, 0, etna2d_surface_width(backingSurf), etna2d_surface_height(backingSurf))
                             .intersected(QRect(0, 0, etna2d_surface_width(scanoutBack),
                                                      etna2d_surface_height(scanoutBack)));

    /* Step 8: non-blocking flip.  While the previous flip is in flight the back buffer is not
     * ours; keep the damage (like a full Wayland ring) and present from the flip-done hook. */
    if (m_screen->asyncFlip()) {
        static const int testDrop = qEnvironmentVariableIntValue("QT_ETNA2D_TEST_DROP");
        static int flushCount = 0;
        const bool fake = testDrop > 0 && ++flushCount % testDrop == 0;
        if (fake || m_screen->flipBusy()) {
            EtnaStats::add(EtnaStats::DroppedFrames);
            accumulateStale(region, bounds);
            m_dropped = true;
            if (fake && !m_retryQueued) {
                m_retryQueued = true;
                QTimer::singleShot(0, [this]() { m_retryQueued = false; presentPending(); });
            }
            BS_DBG << "flush(KMS): flip pending, deferring";
            return;
        }
    }

    /* 1. Unlock: drain CPU writes + GPU fence for any raster-fallback ops. */
    blitter->unlock();

    /* 2. GPU blit only what this scanout buffer is missing (damage tracking:
     *    the back buffer lags the backing store by the regions flushed since it
     *    was last written, so no ghosting). */
    const QRegion damage = takeDamage(scanoutBack, region, bounds);
    BS_DBG << "flush(KMS): blit" << damage.boundingRect() << "(" << damage.rectCount() << "rects)";
    {
        EtnaStats::Scope _t(EtnaStats::FlushBlit);
        blitRegion(ctx, backingSurf, scanoutBack, damage);
    }

    /* 3. Wait for GPU blit to complete before handing the buffer to display. */
    {
        EtnaStats::Scope _t(EtnaStats::FlushFinish);
        if (etna2d_finish(ctx) != ETNA2D_OK) {
            BS_WARN << "flush(KMS): etna2d_finish failed";
            return;
        }
    }

    verifyTarget(backingSurf, scanoutBack, "kms");

    /* 4. Page-flip (blocks until vblank only in composite / QT_ETNA2D_SYNC_FLIP mode). */
    m_dropped = false;
    EtnaStats::Scope _tp(EtnaStats::FlipWait);
    if (m_screen->pageFlip() != 0)
        BS_WARN << "flush(KMS): pageFlip failed";
}

void QEtnaviv2dBackingStore::presentPending()
{
    static const bool off = qEnvironmentVariableIntValue("QT_ETNA2D_NO_REPRESENT") > 0; /* A/B: old behaviour */
    if (off || !m_dropped || !window())
        return;
    EtnaStats::add(EtnaStats::RelPresent);
    BS_DBG << "presenting frame dropped earlier (ring slot available)";
    flush(window(), QRegion(), QPoint());
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
        etna2d_blit(ctx,
                    surf, r.x(),      r.y(),
                    surf, r.x() + dx, r.y() + dy,
                    r.width(), r.height(),
                    ETNA2D_BLEND_NONE, ETNA2D_ROP_COPY);
    }
    etna2d_flush(ctx);

    BS_DBG << "scroll" << area.boundingRect() << "dx=" << dx << "dy=" << dy;
    return true;
}

QT_END_NAMESPACE
