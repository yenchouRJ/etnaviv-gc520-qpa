/*
 * test_glyph.cpp — Risk #3: text rendering (glyph) performance measurement.
 *
 * When Qt's raster engine draws text (or any composited op) onto an
 * EtnaBlittable surface it must READ the destination pixels before blending
 * (SRC_OVER read-modify-write).  On our plugin the destination is a
 * WC-mapped (write-combining) buffer: CPU writes are fast (coalesced) but
 * CPU reads are UNCACHED and slow — every read misses the cache and goes
 * to DRAM.
 *
 * This benchmark measures the real-world impact by timing frames that contain
 * only drawText calls (pure raster SRC_OVER RMW on WC memory) and comparing
 * fps between etnaviv2d and linuxfb.
 *
 * On linuxfb the backing buffer is also a CPU allocation, but it is normal
 * cacheable memory — reads are fast.  The fps difference between the two
 * platforms directly quantifies the WC read penalty for text-heavy UIs.
 *
 * Workloads (selected via --mode=<name>):
 *
 *   text      N calls to drawText (short strings, random positions).
 *             Forces raster glyph blit with SRC_OVER WC reads.   [DEFAULT]
 *
 *   fill      N calls to fillRect (solid opaque colour).
 *             GPU fast path on etnaviv2d (no WC reads); control group.
 *
 *   mixed     N/2 fillRect + N/2 drawText — realistic widget frame.
 *
 * See: qpa/qt-test.md § test_glyph for full criteria and results.
 *
 * Usage:
 *   ./test_glyph [--frames=F] [--count=N] [--mode=text|fill|mixed]
 *
 *   --frames=F   total frames to render before auto-exit  (default: 200)
 *   --count=N    ops per frame                            (default: 20)
 *   --mode=M     workload mode                            (default: text)
 *
 * Example:
 *   QT_QPA_PLATFORM=etnaviv2d ./test_glyph --frames=200 --count=20 --mode=text
 *   QT_QPA_PLATFORM=linuxfb   ./test_glyph --frames=200 --count=20 --mode=text
 *
 * Interpretation:
 *   - If etnaviv2d fps << linuxfb fps in text mode  → WC read penalty is real;
 *     consider a shadow buffer (cacheable copy) for text-heavy UIs.
 *   - If etnaviv2d fps ≈ linuxfb fps in text mode   → WC read cost is acceptable;
 *     risk #3 can be closed as "not significant in practice".
 *   - fill mode should show etnaviv2d fps >= linuxfb fps (GPU fast path wins);
 *     if it does not, there is a regression elsewhere.
 */

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QFont>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QCommandLineParser>
#include <QRect>

#include <cstdlib>  /* rand */

/* -------------------------------------------------------------------------
 * WcReadBenchWidget
 * ------------------------------------------------------------------------- */
class WcReadBenchWidget : public QWidget
{
public:
    enum Mode { ModeText, ModeFill, ModeMixed };

    WcReadBenchWidget(int frames, int count, Mode mode)
        : m_totalFrames(frames)
        , m_count(count)
        , m_mode(mode)
    {
        setWindowTitle(QStringLiteral("test_glyph"));
        m_font.setFamily(QStringLiteral("monospace"));
        m_font.setPixelSize(14);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const int W = width();
        const int H = height();

        /* Black background — GPU fillRect on etnaviv2d, CPU on linuxfb. */
        p.fillRect(rect(), Qt::black);

        if (m_mode == ModeFill || m_mode == ModeMixed) {
            /* Opaque fillRect — exercises GPU fast path on etnaviv2d.
             * Used as control: if this is slow too, regression elsewhere. */
            int n = (m_mode == ModeMixed) ? m_count / 2 : m_count;
            p.setPen(Qt::NoPen);
            for (int i = 0; i < n; ++i) {
                int x = rand() % W;
                int y = rand() % H;
                int w = 40 + rand() % 120;
                int h = 14 + rand() % 30;
                QColor c(rand() % 256, rand() % 256, rand() % 256);
                p.fillRect(QRect(x, y, w, h), c);
            }
        }

        if (m_mode == ModeText || m_mode == ModeMixed) {
            /* drawText — forces raster glyph SRC_OVER onto WC destination.
             * The raster engine reads destination pixels (WC reads) before
             * alpha-blending each glyph row.  This is the hot path for risk #3. */
            int n = (m_mode == ModeMixed) ? m_count / 2 : m_count;
            p.setFont(m_font);
            p.setPen(Qt::white);
            static const char *words[] = {
                "etnaviv2d", "GC520", "Qt5", "raster", "WC", "blitter",
                "QPainter", "QPixmap", "QImage", "MA35D1", "nuvoton", "drm"
            };
            constexpr int nwords = sizeof(words) / sizeof(words[0]);
            for (int i = 0; i < n; ++i) {
                int x = rand() % (W - 100);
                int y = 14 + rand() % (H - 14);
                p.drawText(x, y, QString::fromLatin1(words[i % nwords]));
            }
        }

        /* Status overlay — drawn last so it's always visible. */
        p.setPen(Qt::yellow);
        p.setFont(m_font);
        p.drawText(4, 16, QStringLiteral("frame %1/%2  count=%3  mode=%4")
                          .arg(m_frame + 1).arg(m_totalFrames)
                          .arg(m_count).arg(modeName()));
    }

    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        /* Start rendering loop after the first show. */
        if (!m_started) {
            m_started = true;
            m_elapsed.start();
            QTimer::singleShot(0, this, &WcReadBenchWidget::nextFrame);
        }
    }

private slots:
    void nextFrame()
    {
        if (m_frame >= m_totalFrames) {
            report();
            QApplication::quit();
            return;
        }
        update();
        ++m_frame;
        QTimer::singleShot(0, this, &WcReadBenchWidget::nextFrame);
    }

private:
    void report()
    {
        qint64 ms = m_elapsed.elapsed();
        double fps = m_frame * 1000.0 / ms;
        double mspf = ms / double(m_frame);
        qDebug().noquote()
            << QStringLiteral(
               "platform=%1  mode=%2  frames=%3  count=%4  "
               "elapsed=%5ms  fps=%6  ms/frame=%7")
               .arg(QApplication::platformName())
               .arg(modeName())
               .arg(m_frame)
               .arg(m_count)
               .arg(ms)
               .arg(QString::number(fps,  'f', 1))
               .arg(QString::number(mspf, 'f', 2));
    }

    QString modeName() const
    {
        switch (m_mode) {
        case ModeText:   return QStringLiteral("text");
        case ModeFill:   return QStringLiteral("fill");
        case ModeMixed:  return QStringLiteral("mixed");
        }
        return {};
    }

    int           m_totalFrames;
    int           m_count;
    Mode          m_mode;
    int           m_frame   = 0;
    bool          m_started = false;
    QFont         m_font;
    QElapsedTimer m_elapsed;
};

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QCommandLineParser cli;
    cli.addOption({ QStringLiteral("frames"), QStringLiteral("total frames"),
                    QStringLiteral("N"), QStringLiteral("200") });
    cli.addOption({ QStringLiteral("count"),  QStringLiteral("ops per frame"),
                    QStringLiteral("N"), QStringLiteral("20") });
    cli.addOption({ QStringLiteral("mode"),
                    QStringLiteral("workload: text|fill|mixed"),
                    QStringLiteral("M"), QStringLiteral("text") });
    cli.process(app);

    int frames = cli.value(QStringLiteral("frames")).toInt();
    int count  = cli.value(QStringLiteral("count")).toInt();
    if (frames < 1) frames = 200;
    if (count  < 1) count  = 20;

    WcReadBenchWidget::Mode mode = WcReadBenchWidget::ModeText;
    QString modeStr = cli.value(QStringLiteral("mode"));
    if (modeStr == QLatin1String("fill"))  mode = WcReadBenchWidget::ModeFill;
    if (modeStr == QLatin1String("mixed")) mode = WcReadBenchWidget::ModeMixed;

    qDebug().noquote()
        << QStringLiteral("platform=%1  frames=%2  count=%3  mode=%4")
           .arg(QApplication::platformName()).arg(frames).arg(count)
           .arg(modeStr);

    WcReadBenchWidget w(frames, count, mode);
    w.showFullScreen();

    return app.exec();
}
