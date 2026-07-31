/*
 * test_slideshow.cpp — Risk #2: raster-source pixmap path on hardware.
 *
 * QImage (CPU buffer) and QPixmap (EtnaBlittable GPU surface) are different
 * objects.  Drawing a QPixmap onto another QPixmap goes through the GPU
 * blitter (etna2d_blit).  Drawing a QImage forces the CPU raster engine to
 * write directly into the WC-mapped backing buffer — the "raster fallback".
 *
 * This test shows both paths side-by-side so any pixel difference (corruption,
 * colour shift, cache-ordering artefact) is immediately visible.
 *
 * Layout — screen split into two equal halves:
 *
 * ┌──────────────────────┬──────────────────────┐
 * │  QPixmap / blitter   │  QImage / raster     │
 * │                      │                      │
 * │      [image]         │      [image]         │
 * │                      │                      │
 * │  frame N  parrot-N.png  interval=800ms      │
 * └──────────────────────┴──────────────────────┘
 *      left half                right half
 *
 * Pass: both halves look identical → raster fallback writes correct pixels;
 *       CPU→GPU barrier in doUnlock is sufficient; format is consistent.
 * See: qpa/qt-test.md § test_slideshow for full criteria and proof summary.
 *
 * Usage:
 *   ./test_slideshow [image_dir [interval_ms [loops]]]
 *
 *   image_dir    directory containing PNG/JPEG files  (default: ../../images)
 *   interval_ms  ms between slides                    (default: 800)
 *   loops        how many full cycles before auto-exit (default: 2, 0=infinite)
 *
 * Example:
 *   QT_QPA_PLATFORM=etnaviv2d ./test_slideshow ../../images 800 3
 *   QT_QPA_PLATFORM=linuxfb   ./test_slideshow ../../images 800 3
 */

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QDebug>
#include <QElapsedTimer>

/* -------------------------------------------------------------------------
 * Helper: compute centred destination rect that fits src into panel,
 * preserving aspect ratio.
 * ------------------------------------------------------------------------- */
static QRect fitRect(const QSize &src, const QRect &panel)
{
    if (src.isEmpty() || panel.isEmpty())
        return panel;

    QSize scaled = src.scaled(panel.size(), Qt::KeepAspectRatio);
    int x = panel.x() + (panel.width()  - scaled.width())  / 2;
    int y = panel.y() + (panel.height() - scaled.height()) / 2;
    return QRect(x, y, scaled.width(), scaled.height());
}

/* -------------------------------------------------------------------------
 * SlideshowWidget
 * ------------------------------------------------------------------------- */
class SlideshowWidget : public QWidget
{
public:
    SlideshowWidget(const QStringList &paths, int intervalMs, int loops)
        : m_paths(paths)
        , m_loops(loops)
    {
        setWindowTitle(QStringLiteral("test_slideshow"));

        qDebug() << "Loading" << paths.size() << "images...";
        for (const QString &p : paths) {
            QImage img;
            if (!img.load(p)) {
                qWarning() << "  FAILED to load:" << p;
                continue;
            }
            img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            m_pixmaps.append(QPixmap::fromImage(img));
            m_rawImages.append(img);
            qDebug() << "  loaded:" << QFileInfo(p).fileName()
                     << img.width() << "x" << img.height();
        }

        if (m_pixmaps.isEmpty())
            qFatal("No images loaded — aborting");

        m_elapsed.start();

        m_timer = new QTimer(this);
        m_timer->setInterval(intervalMs);
        connect(m_timer, &QTimer::timeout, this, &SlideshowWidget::advance);
        m_timer->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);

        const int W = width();
        const int H = height();
        const int halfW = W / 2;

        /* Black background for the whole widget. */
        p.fillRect(rect(), Qt::black);

        const int n = m_pixmaps.size();
        const int idx = m_frame % n;

        /* --- Left half: QPixmap (blitter→blitter GPU path) --- */
        QRect leftPanel(0, 0, halfW, H);
        QRect leftDst = fitRect(m_pixmaps[idx].size(), leftPanel);
        p.drawPixmap(leftDst, m_pixmaps[idx], m_pixmaps[idx].rect());

        /* --- Right half: QImage (raster → WC-mapped BO CPU fallback) --- */
        QRect rightPanel(halfW, 0, W - halfW, H);
        QRect rightDst = fitRect(m_rawImages[idx].size(), rightPanel);
        p.drawImage(rightDst, m_rawImages[idx], m_rawImages[idx].rect());

        /* --- Divider line --- */
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(halfW, 0, halfW, H);

        /* --- Labels --- */
        p.setPen(Qt::white);
        p.setFont(QFont(QStringLiteral("monospace"), 11));
        p.drawText(leftPanel.adjusted(4, 4, -4, -4),
                   Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("QPixmap / blitter"));
        p.drawText(rightPanel.adjusted(4, 4, -4, -4),
                   Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("QImage / raster"));

        /* --- Bottom status bar --- */
        QString status = QStringLiteral("frame %1  %2  interval=%3ms")
                         .arg(m_frame)
                         .arg(QFileInfo(m_paths[idx]).fileName())
                         .arg(m_timer->interval());
        p.drawText(rect().adjusted(4, 0, -4, -4),
                   Qt::AlignBottom | Qt::AlignLeft, status);
    }

private slots:
    void advance()
    {
        ++m_frame;

        const int n = m_pixmaps.size();
        if (m_loops > 0 && m_frame >= n * m_loops) {
            qint64 ms = m_elapsed.elapsed();
            qDebug().noquote()
                << QStringLiteral("slideshow done: frames=%1  elapsed=%2ms  fps=%3")
                   .arg(m_frame).arg(ms)
                   .arg(QString::number(m_frame * 1000.0 / ms, 'f', 1));
            m_timer->stop();
            QApplication::quit();
            return;
        }

        update();
    }

private:
    QStringList    m_paths;
    QList<QPixmap> m_pixmaps;
    QList<QImage>  m_rawImages;
    QTimer        *m_timer = nullptr;
    QElapsedTimer  m_elapsed;
    int            m_frame = 0;
    int            m_loops;
};

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QString imageDir   = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("../../images");
    int     intervalMs = argc > 2 ? QByteArray(argv[2]).toInt() : 800;
    int     loops      = argc > 3 ? QByteArray(argv[3]).toInt() : 2;

    if (intervalMs < 16) intervalMs = 16;

    QDir dir(imageDir);
    if (!dir.exists())
        qFatal("Image directory not found: %s", qPrintable(imageDir));

    QStringList filters = { QStringLiteral("*.png"), QStringLiteral("*.jpg"),
                             QStringLiteral("*.jpeg") };
    QStringList files = dir.entryList(filters, QDir::Files, QDir::Name);
    if (files.isEmpty())
        qFatal("No PNG/JPEG files found in: %s", qPrintable(imageDir));

    QStringList paths;
    paths.reserve(files.size());
    for (const QString &f : files)
        paths.append(dir.absoluteFilePath(f));

    qDebug().noquote()
        << QStringLiteral("platform=%1  dir=%2  images=%3  interval=%4ms  loops=%5")
           .arg(QApplication::platformName())
           .arg(imageDir).arg(paths.size()).arg(intervalMs)
           .arg(loops > 0 ? QString::number(loops) : QStringLiteral("inf"));

    SlideshowWidget w(paths, intervalMs, loops);
    w.showFullScreen();

    return app.exec();
}
