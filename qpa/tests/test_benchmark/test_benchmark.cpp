/*
 * test_benchmark.cpp — Step 4: GPU vs CPU frame-rate benchmark.
 *
 * Mimics a widget application rendering N sprites per frame and measures
 * actual achieved FPS for:
 *   (a) etnaviv2d plugin — GPU fill + blit compositing
 *   (b) linuxfb plugin   — CPU raster baseline
 *
 * Two compositing modes (selected by QT_ETNAVIV2D_BENCH_OPAQUE):
 *   0 (default) — alpha=200 translucent sprites, Format_ARGB32_Premultiplied.
 *                 Both platforms do a full SRC_OVER alpha blend.
 *                 This is the fair apples-to-apples comparison.
 *   1           — alpha=255 fully-opaque sprites.
 *                 linuxfb can use a plain memcpy; etnaviv2d uses ETNA2D_BLEND_NONE.
 *                 Measures raw blit throughput without blending overhead.
 *
 * Run modes:
 *   QT_QPA_PLATFORM=etnaviv2d  ./test_benchmark [nframes] [nsprites]
 *   QT_QPA_PLATFORM=linuxfb   ./test_benchmark [nframes] [nsprites]
 *   QT_ETNAVIV2D_BENCH_OPAQUE=1  QT_QPA_PLATFORM=etnaviv2d ./test_benchmark ...
 *
 * Output:
 *   platform=X  frames=N  sprites=M  mode=alpha|opaque  elapsed=X.XXXs  fps=YY.Y  ms/frame=Z.ZZ
 *
 * Defaults: nframes=120, nsprites=32.
 *
 * The benchmark uses QPainter exclusively (no direct libetna2d calls) so
 * it exercises exactly the QPA path a real application uses.
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QElapsedTimer>
#include <QTimer>
#include <QDebug>
#include <cstdio>
#include <cstdlib>

/* -----------------------------------------------------------------------
 * Benchmark widget
 * ----------------------------------------------------------------------- */
class BenchWidget : public QWidget
{
public:
    BenchWidget(int nframes, int nsprites, bool opaque, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_totalFrames(nframes)
        , m_nsprites(nsprites)
        , m_opaque(opaque)
    {
        setWindowTitle(QStringLiteral("etnaviv2d benchmark"));

        /* Build sprite pixmap.
         * Always use Format_ARGB32_Premultiplied so both etnaviv2d and linuxfb
         * go through the same alpha-compositing code path (SRC_OVER blend).
         * With opaque=true, alpha=255 so the blend degenerates to a plain copy
         * on both platforms — measuring raw blit throughput. */
        const int alpha = opaque ? 255 : 200;
        QImage img(96, 96, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&img);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect( 0,  0, 48, 48, QColor(255,  64,  64, alpha));
            p.fillRect(48,  0, 48, 48, QColor( 64, 255,  64, alpha));
            p.fillRect( 0, 48, 48, 48, QColor( 64,  64, 255, alpha));
            p.fillRect(48, 48, 48, 48, QColor(255, 255,  64, alpha));
        }
        m_sprite = QPixmap::fromImage(img);

        /* Build sprite positions (deterministic, wrapping). */
        m_positions.reserve(nsprites);
        for (int i = 0; i < nsprites; ++i)
            m_positions.append(QPoint((i * 37) % 900, (i * 53) % 550));
    }

    void startBenchmark()
    {
        m_frame = 0;
        m_elapsed.start();
        QTimer::singleShot(0, this, &BenchWidget::nextFrame);
    }

    int    totalFrames() const { return m_totalFrames; }
    qint64 elapsedMs()   const { return m_elapsed.elapsed(); }
    bool   isOpaque()    const { return m_opaque; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        int w = width(), h = height();

        /* Background fill. */
        p.fillRect(0, 0, w, h, QColor(24, 24, 24));

        /* Animate sprites: offset by frame number. */
        int step = m_frame % 200;
        for (int i = 0; i < m_nsprites; ++i) {
            int x = (m_positions[i].x() + step) % w;
            int y = (m_positions[i].y() + step / 2) % h;
            x = qMin(x, w - 96);
            y = qMin(y, h - 96);
            p.drawPixmap(x, y, m_sprite);
        }

        /* Frame counter label (raster fallback on both platforms). */
        p.setPen(Qt::white);
        p.drawText(10, 20, QStringLiteral("Frame %1 / %2")
                            .arg(m_frame).arg(m_totalFrames));
    }

private slots:
    void nextFrame()
    {
        update();
        ++m_frame;
        if (m_frame < m_totalFrames)
            QTimer::singleShot(0, this, &BenchWidget::nextFrame);
        else
            QApplication::quit();
    }

private:
    int             m_totalFrames;
    int             m_nsprites;
    bool            m_opaque;
    int             m_frame = 0;
    QPixmap         m_sprite;
    QVector<QPoint> m_positions;
    QElapsedTimer   m_elapsed;
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int nframes  = (argc > 1) ? atoi(argv[1]) : 120;
    int nsprites = (argc > 2) ? atoi(argv[2]) : 32;
    if (nframes  < 1) nframes  = 120;
    if (nsprites < 1) nsprites = 32;

    bool opaque = qEnvironmentVariableIntValue("QT_ETNAVIV2D_BENCH_OPAQUE") != 0;

    QApplication app(argc, argv);

    printf("platform=%s  frames=%d  sprites=%d  mode=%s\n",
           qPrintable(QGuiApplication::platformName()),
           nframes, nsprites,
           opaque ? "opaque" : "alpha");

    BenchWidget w(nframes, nsprites, opaque);
    w.showFullScreen();
    w.startBenchmark();

    int rc = app.exec();

    qint64 ms  = w.elapsedMs();
    double fps  = (ms > 0) ? (w.totalFrames() * 1000.0 / ms) : 0.0;
    double mspf = (w.totalFrames() > 0) ? double(ms) / w.totalFrames() : 0.0;

    printf("frames=%d  sprites=%d  elapsed=%.3fs  fps=%.1f  ms/frame=%.2f\n",
           w.totalFrames(), nsprites, ms / 1000.0, fps, mspf);

    return rc;
}
