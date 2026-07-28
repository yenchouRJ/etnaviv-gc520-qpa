/*
 * qetnaviv2dbackingstore.h — QPlatformBackingStore for the etnaviv2d plugin.
 *
 * Strategy (option B, decided in plan.md):
 *  - paintDevice() returns a QPixmap backed by an EtnaBlittablePlatformPixmap
 *    (an offscreen GPU surface).  Qt paints into it via QBlitterPaintEngine.
 *  - flush() blits the offscreen backing BO into the current scanout back-
 *    buffer via etna2d_blit(), then calls screen->pageFlip().
 *
 * This keeps the flush path simple and avoids vsync/locking races that arise
 * from painting directly into the scanout BO.  A zero-copy upgrade (option A)
 * can be introduced later by replacing the offscreen surface with the scanout
 * surface in resize().
 */
#ifndef QETNAVIV2DBACKINGSTORE_H
#define QETNAVIV2DBACKINGSTORE_H

#include <qpa/qplatformbackingstore.h>
#include <QtGui/private/qpixmap_blitter_p.h>

#include "qetnaviv2dblitter.h"
#include "qetnaviv2dscreen.h"

QT_BEGIN_NAMESPACE

class QEtnaviv2dBackingStore : public QPlatformBackingStore
{
public:
    explicit QEtnaviv2dBackingStore(QWindow *window,
                                    QEtnaviv2dScreen *screen);
    ~QEtnaviv2dBackingStore() override;

    /* Qt paints into this device. */
    QPaintDevice *paintDevice() override;

    /* Blit backing surface → scanout back-buffer, then page-flip. */
    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override;

    /* Reallocate the backing surface when the window is resized. */
    void resize(const QSize &size, const QRegion &staticContents) override;

    /* For screenshots / accessibility. */
    QImage toImage() const override;

    /* Scroll acceleration: GPU blit within the backing surface. */
    bool scroll(const QRegion &area, int dx, int dy) override;

private:
    QEtnaviv2dScreen         *m_screen;  /* borrowed */
    QScopedPointer<QPixmap>   m_pixmap;  /* wraps the blittable */
    EtnaBlittablePlatformPixmap *m_pmdata = nullptr; /* raw ptr into m_pixmap */
};

QT_END_NAMESPACE

#endif // QETNAVIV2DBACKINGSTORE_H
