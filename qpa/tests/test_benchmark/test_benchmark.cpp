/*
 * test_benchmark.cpp — Step 4: GPU vs CPU frame-rate benchmark.
 *
 * Mimics a widget application rendering N sprites per frame at 60 Hz
 * and measures actual achieved FPS for:
 *   (a) etnaviv2d plugin — GPU fill + blit compositing
 *   (b) linuxfb plugin   — CPU raster baseline
 *
 * Run modes:
 *   QT_QPA_PLATFORM=etnaviv2d  ./test_benchmark [nframes] [nsprites]
 *   QT_QPA_PLATFORM=linuxfb   ./test_benchmark [nframes] [nsprites]
 *
 * Output:
 *   frames=N  sprites=M  elapsed=X.XXXs  fps=YY.Y  ms/frame=Z.ZZ
 *
 * Defaults: nframes=120 (2 seconds @ 60 Hz), nsprites=32 (96×96 each).
 *
 * The benchmark uses QPainter exclusively (no direct libetna2d calls) so
 * it exercises exactly the QPA path a real application uses.
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
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
    BenchWidget(int nframes, int nsprites, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_totalFrames(nframes)
        , m_nsprites(nsprites)
    {
        setWindowTitle(QStringLiteral("etnaviv2d benchmark"));

        /* Build sprite pixmap: 96x96 premultiplied colour wheel pattern. */
        m_sprite = QPixmap(96, 96);
        QPainter p(&m_sprite);
        p.fillRect( 0,  0, 48, 48, QColor(255,  64,  64, 200)); /* red    */
        p.fillRect(48,  0, 48, 48, QColor( 64, 255,  64, 200)); /* green  */
        p.fillRect( 0, 48, 48, 48, QColor( 64,  64, 255, 200)); /* blue   */
        p.fillRect(48, 48, 48, 48, QColor(255, 255,  64, 200)); /* yellow */

        /* Build sprite positions (deterministic, wrapping). */
        m_positions.reserve(nsprites);
        for (int i = 0; i < nsprites; ++i) {
            m_positions.append(QPoint((i * 37) % 900, (i * 53) % 550));
        }
    }

    void startBenchmark()
    {
        m_frame   = 0;
        m_elapsed.start();
        QTimer::singleShot(0, this, &BenchWidget::nextFrame);
    }

    int   totalFrames() const { return m_totalFrames; }
    qint64 elapsedMs()  const { return m_elapsed.elapsed(); }

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
            /* Clamp so sprite is fully on-screen (avoids MMU fault risk). */
            x = qMin(x, w - 96);
            y = qMin(y, h - 96);
            p.drawPixmap(x, y, m_sprite);
        }

        /* Frame counter label (raster fallback). */
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
    int              m_totalFrames;
    int              m_nsprites;
    int              m_frame = 0;
    QPixmap          m_sprite;
    QVector<QPoint>  m_positions;
    QElapsedTimer    m_elapsed;
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

    QApplication app(argc, argv);

    printf("platform=%s  frames=%d  sprites=%d\n",
           qPrintable(QGuiApplication::platformName()),
           nframes, nsprites);

    BenchWidget w(nframes, nsprites);
    w.showFullScreen();
    w.startBenchmark();

    int rc = app.exec();

    qint64 ms = w.elapsedMs();
    double fps = (ms > 0) ? (w.totalFrames() * 1000.0 / ms) : 0.0;
    double mspf = (w.totalFrames() > 0) ? double(ms) / w.totalFrames() : 0.0;

    printf("frames=%d  sprites=%d  elapsed=%.3fs  fps=%.1f  ms/frame=%.2f\n",
           w.totalFrames(), nsprites, ms / 1000.0, fps, mspf);

    return rc;
}
