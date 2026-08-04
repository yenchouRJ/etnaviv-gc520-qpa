/*
 * test_kms_thread.cpp — Risk #5: KMS page-flip thread safety stress test.
 *
 * QEtnaviv2dScreen::pageFlip() touches m_kfd, m_flipPending, and m_backIdx
 * without any lock.  When Qt's render thread (QT_THREADED_PAINTING=1) calls
 * flush() concurrently with the GUI thread, two callers can reach
 * drmModePageFlip simultaneously — one while a flip is already pending —
 * producing EBUSY, torn frames, or a crash during teardown.
 *
 * Layout (1024×600):
 *
 *   +-------------------------+-------------------------+
 *   |      Thread A           |      Thread B           |
 *   |      (Left half)        |      (Right half)       |
 *   |                         |                         |
 *   |  earth.mp4 playback     |  Bouncing alpha circles |
 *   |  pre-extracted PNGs     |  unthrottled — maximum  |
 *   |  throttled to --fps     |  flush() contention     |
 *   |                         |                         |
 *   |  High-contrast edges    |  Real-time FPS counter  |
 *   |  (coastlines, clouds)   |  + frame counter        |
 *   |  make tearing obvious   |                         |
 *   +-------------------------+-------------------------+
 *
 * --circles=0  skips Thread B entirely (single-thread baseline).
 *
 * Memory model:
 *   Frames are loaded as QImage (CPU RAM) via a sliding ring-buffer of
 *   --cache frames.  QPainter::drawImage() composites directly onto the
 *   backing surface — no QPixmap::fromImage(), no per-frame etna_bo.
 *   --cache=2 is recommended on MA35D1: minimises decoded-frame backlog
 *   that can cause jitter without stalling on disk reads.
 *
 * Usage:
 *   QT_QPA_PLATFORM=etnaviv2d ./test_kms_thread \
 *       --frames=100 --fps=30 --circles=10 \
 *       --frames-dir=./earth_frames/ --cache=2
 *
 * Race exposure (before fix):
 *   QT_QPA_PLATFORM=etnaviv2d QT_THREADED_PAINTING=1 ./test_kms_thread \
 *       --frames=100 --fps=30 --circles=10 \
 *       --frames-dir=./earth_frames/ --cache=2
 *   → visible horizontal tearing on left half; EBUSY in dmesg
 *
 * After fix (m_flipMutex + m_shuttingDown in qetnaviv2dscreen.cpp):
 *   Same command → no tearing, smooth FPS, clean exit.
 */

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QThread>
#include <QMutex>
#include <QElapsedTimer>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <cstdio>
#include <cstdlib>
#include <cmath>

/* -----------------------------------------------------------------------
 * Configuration parsed from argv
 * ----------------------------------------------------------------------- */
struct Config {
    int     totalFrames = 300;
    int     fps         = 60;   /* target fps for Thread A (video side) */
    int     circles     = 16;   /* number of bouncing circles (Thread B) */
    int     cacheFrames = 32;   /* sliding window of decoded frames kept in RAM */
    QString framesDir;          /* path to earth_frames/ */
};

/* -----------------------------------------------------------------------
 * Shared state between threads — no mutex (that is the point of the test)
 * ----------------------------------------------------------------------- */
struct SharedState {
    /* Thread A writes, Thread B reads for tearing detection overlay. */
    volatile int videoFrame  = 0;
    volatile int videoFps    = 0;

    /* Thread B writes. */
    volatile int circleFrame = 0;
    volatile int circleFps   = 0;

    /* Set by either thread when done. */
    volatile bool done = false;
};

/* -----------------------------------------------------------------------
 * VideoHalf — painted by Thread A into the left half
 *
 * Memory strategy: load frames as QImage (CPU RAM) on demand using a
 * sliding ring-buffer of size cfg.cacheFrames.  Never holds more than
 * cacheFrames decoded images at once — avoids allocating 500+ MB of GPU
 * BOs when running under etnaviv2d (where QPixmap maps to an etna_bo).
 * The ring index is (globalFrameNumber % totalDiskFrames); when we advance
 * past the tail of the ring we drop the oldest entry.
 * ----------------------------------------------------------------------- */
class VideoHalf : public QObject
{
    Q_OBJECT
public:
    VideoHalf(QWidget *canvas, const Config &cfg, SharedState *st)
        : m_canvas(canvas), m_cfg(cfg), m_state(st)
    {
        QDir dir(m_cfg.framesDir);
        m_files = dir.entryList(
            QStringList() << "frame_*.png", QDir::Files, QDir::Name);
        m_totalDiskFrames = m_files.size();
        printf("[video] found %d frames on disk  cache=%d\n",
               m_totalDiskFrames, m_cfg.cacheFrames);
    }

public slots:
    void run()
    {
        if (m_totalDiskFrames == 0) {
            fprintf(stderr, "[video] no frames found in %s\n",
                    qPrintable(m_cfg.framesDir));
            m_state->done = true;
            emit finished();
            return;
        }

        QDir dir(m_cfg.framesDir);
        const int intervalMs = (m_cfg.fps > 0) ? (1000 / m_cfg.fps) : 16;
        int  fpsCount = 0;
        QElapsedTimer fpsClock, total;
        fpsClock.start();
        total.start();

        /* Pre-fill the cache with the first cacheFrames frames. */
        int cacheSize = qMin(m_cfg.cacheFrames, m_totalDiskFrames);
        m_cache.reserve(cacheSize);
        for (int i = 0; i < cacheSize; ++i) {
            m_cache.append(QImage(dir.filePath(m_files[i])));
        }
        m_cacheStart = 0;  /* disk index of m_cache[0] */

        while (m_state->videoFrame < m_cfg.totalFrames && !m_state->done) {
            QElapsedTimer frameTimer;
            frameTimer.start();

            int diskIdx = m_state->videoFrame % m_totalDiskFrames;

            /* Slide the cache window forward if needed. */
            int cacheEnd = m_cacheStart + m_cache.size();  /* exclusive */
            if (diskIdx >= cacheEnd || diskIdx < m_cacheStart) {
                /* Re-centre window on diskIdx. */
                m_cacheStart = diskIdx;
                int newSize = qMin(m_cfg.cacheFrames,
                                   m_totalDiskFrames - m_cacheStart);
                m_cache.resize(newSize);
                for (int i = 0; i < newSize; ++i) {
                    int fi = (m_cacheStart + i) % m_totalDiskFrames;
                    m_cache[i] = QImage(dir.filePath(m_files[fi]));
                }
                cacheEnd = m_cacheStart + m_cache.size();
            }

            int cacheIdx = diskIdx - m_cacheStart;
            emit paintLeft(m_cache[cacheIdx], m_state->videoFrame);

            ++m_state->videoFrame;
            ++fpsCount;
            if (fpsClock.elapsed() >= 1000) {
                m_state->videoFps = fpsCount;
                fpsCount = 0;
                fpsClock.restart();
            }

            qint64 elapsed = frameTimer.elapsed();
            if (elapsed < intervalMs)
                QThread::msleep(static_cast<unsigned long>(intervalMs - elapsed));
        }

        m_state->done = true;
        printf("[video] done — %d frames in %.1f s\n",
               m_state->videoFrame, total.elapsed() / 1000.0);
        emit finished();
    }

signals:
    void paintLeft(const QImage &frame, int frameNum);
    void finished();

private:
    QWidget      *m_canvas;
    Config        m_cfg;
    SharedState  *m_state;
    QStringList   m_files;
    int           m_totalDiskFrames = 0;
    QVector<QImage> m_cache;
    int           m_cacheStart = 0;  /* disk index of m_cache[0] */
};

/* -----------------------------------------------------------------------
 * Circle POD — shared between CircleHalf and MainWidget
 * ----------------------------------------------------------------------- */
struct Circle {
    float  x, y, vx, vy;
    int    r;
    QColor color;
};

Q_DECLARE_METATYPE(QVector<Circle>)
Q_DECLARE_METATYPE(QPixmap)

/* -----------------------------------------------------------------------
 * CircleHalf — painted by Thread B into the right half, unthrottled
 * ----------------------------------------------------------------------- */
class CircleHalf : public QObject
{
    Q_OBJECT
public:
    CircleHalf(QWidget *canvas, const Config &cfg, SharedState *st)
        : m_canvas(canvas), m_cfg(cfg), m_state(st)
    {
        /* Initialise circles: position, velocity, radius, color. */
        const int rw = canvas->width() / 2;
        const int rh = canvas->height();
        m_circles.resize(cfg.circles);
        for (int i = 0; i < cfg.circles; ++i) {
            Circle &c = m_circles[i];
            c.r    = 20 + (i * 7) % 30;
            c.x    = c.r + (i * 53) % (rw - 2 * c.r);
            c.y    = c.r + (i * 37) % (rh - 2 * c.r);
            c.vx   = 1.5f + (i % 5) * 0.7f;
            c.vy   = 1.2f + (i % 4) * 0.6f;
            /* Distinct hue per circle, alpha=180 for blend stress. */
            float hue = float(i) / cfg.circles;
            c.color = QColor::fromHsvF(hue, 0.9f, 1.0f, 180.0f / 255.0f);
        }
    }

public slots:
    void run()
    {
        const int rw = m_canvas->width() / 2;
        const int rh = m_canvas->height();
        int  fpsCount = 0;
        QElapsedTimer fpsClock;
        fpsClock.start();

        while (!m_state->done) {
            /* Advance physics. */
            for (Circle &c : m_circles) {
                c.x += c.vx;
                c.y += c.vy;
                if (c.x - c.r < 0)   { c.x =  c.r;      c.vx = -c.vx; }
                if (c.x + c.r > rw)  { c.x = rw - c.r;  c.vx = -c.vx; }
                if (c.y - c.r < 0)   { c.y =  c.r;      c.vy = -c.vy; }
                if (c.y + c.r > rh)  { c.y = rh - c.r;  c.vy = -c.vy; }
            }

            emit paintRight(m_circles, m_state->circleFrame,
                            m_state->circleFps, m_state->videoFps);

            ++m_state->circleFrame;
            ++fpsCount;
            if (fpsClock.elapsed() >= 1000) {
                m_state->circleFps = fpsCount;
                fpsCount = 0;
                fpsClock.restart();
            }
        }

        emit finished();
    }

signals:
    void paintRight(const QVector<Circle> &circles,
                    int frame, int circleFps, int videoFps);
    void finished();

private:
    QWidget        *m_canvas;
    Config          m_cfg;
    SharedState    *m_state;
    QVector<Circle> m_circles;
};

/* -----------------------------------------------------------------------
 * MainWidget — receives paint signals from both threads, composes, flushes
 * ----------------------------------------------------------------------- */
class MainWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MainWidget(const Config &cfg, QWidget *parent = nullptr)
        : QWidget(parent), m_cfg(cfg)
    {
        setWindowTitle("test_kms_thread — Risk #5");
    }

public slots:
    /* Called from Thread A via queued connection → GUI thread. */
    void onPaintLeft(const QImage &frame, int frameNum)
    {
        m_videoFrame  = frame;
        m_videoFrameN = frameNum;
        update();
    }

    /* Called from Thread B via queued connection → GUI thread. */
    void onPaintRight(const QVector<Circle> &circles,
                      int frame, int circleFps, int videoFps)
    {
        m_circles    = circles;
        m_circleFrame = frame;
        m_circleFps  = circleFps;
        m_videoFps   = videoFps;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const int w  = width();
        const int h  = height();
        const int hw = w / 2;

        /* ---- Left half: video frame ---- */
        p.fillRect(0, 0, hw, h, Qt::black);
        if (!m_videoFrame.isNull()) {
            /* Draw QImage directly — avoids QPixmap::fromImage() which would
             * allocate a new etna_bo (GPU BO) on every frame under etnaviv2d,
             * exhausting the CMA heap.  QPainter::drawImage() composites via
             * the raster engine onto the backing surface (CPU→CPU path). */
            QImage scaled = m_videoFrame.scaled(
                hw, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            int ox = (hw - scaled.width())  / 2;
            int oy = (h  - scaled.height()) / 2;
            p.drawImage(ox, oy, scaled);
        }

        /* Video FPS label. */
        p.setPen(QColor(255, 220, 0));
        p.drawText(8, 20, QString("Video  frame=%1  fps=%2")
                   .arg(m_videoFrameN).arg(m_videoFps));

        /* ---- Right half: bouncing circles ---- */
        p.fillRect(hw, 0, hw, h, QColor(20, 20, 30));

        p.translate(hw, 0);
        p.setRenderHint(QPainter::Antialiasing);
        for (const Circle &c : m_circles) {
            p.setBrush(c.color);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(c.x, c.y), c.r, c.r);
        }
        p.setRenderHint(QPainter::Antialiasing, false);

        /* Circle FPS + frame counter. */
        p.setPen(Qt::white);
        p.drawText(8, 20, QString("Circles  frame=%1  fps=%2")
                   .arg(m_circleFrame).arg(m_circleFps));
        p.drawText(8, 40, QString("Video fps=%1").arg(m_videoFps));
        p.resetTransform();
    }

private:
    Config  m_cfg;
    QImage  m_videoFrame;
    int     m_videoFrameN = 0;
    int     m_videoFps    = 0;
    QVector<Circle> m_circles;
    int     m_circleFrame = 0;
    int     m_circleFps   = 0;
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    qRegisterMetaType<QVector<Circle>>();
    qRegisterMetaType<QImage>();

    Config cfg;

    /* Parse arguments. */
    for (int i = 1; i < argc; ++i) {
        QString arg(argv[i]);
        if (arg.startsWith("--frames="))
            cfg.totalFrames = arg.mid(9).toInt();
        else if (arg.startsWith("--fps="))
            cfg.fps = arg.mid(6).toInt();
        else if (arg.startsWith("--circles="))
            cfg.circles = arg.mid(10).toInt();
        else if (arg.startsWith("--cache="))
            cfg.cacheFrames = arg.mid(8).toInt();
        else if (arg.startsWith("--frames-dir="))
            cfg.framesDir = arg.mid(13);
    }

    /* Default frames dir: relative to binary location. */
    if (cfg.framesDir.isEmpty()) {
        /* Try the canonical install path relative to the test binary. */
        QString binDir = QFileInfo(argv[0]).absolutePath();
        /* Walk up to qpa/ and look for images/earth_frames. */
        QDir d(binDir);
        for (int up = 0; up < 5; ++up) {
            QString candidate = d.filePath("images/earth_frames");
            if (QDir(candidate).exists()) {
                cfg.framesDir = candidate;
                break;
            }
            if (!d.cdUp()) break;
        }
    }
    if (cfg.framesDir.isEmpty() || !QDir(cfg.framesDir).exists()) {
        fprintf(stderr,
            "error: earth_frames not found.\n"
            "Run:  ffmpeg -i qpa/images/earth.mp4 -vf scale=512:300 \\\n"
            "            qpa/images/earth_frames/frame_%%04d.png\n"
            "Or pass --frames-dir=<path>\n");
        return 1;
    }

    if (cfg.cacheFrames < 2)  cfg.cacheFrames = 2;
    if (cfg.totalFrames < 1) cfg.totalFrames = 300;
    if (cfg.fps < 1)         cfg.fps = 60;
    if (cfg.circles < 0)     cfg.circles = 0;  /* 0 = no stress thread (baseline) */

    printf("platform=%s  frames=%d  fps=%d  circles=%d  cache=%d  mode=%s\n",
           qPrintable(QGuiApplication::platformName()),
           cfg.totalFrames, cfg.fps, cfg.circles, cfg.cacheFrames,
           cfg.circles == 0 ? "baseline(single-thread)" : "stress(dual-thread)");
    printf("frames-dir=%s\n", qPrintable(cfg.framesDir));

    QApplication app(argc, argv);

    SharedState state;
    MainWidget  widget(cfg);
    widget.showFullScreen();

    /* --- Thread A: video playback --- */
    QThread      threadA;
    VideoHalf    videoHalf(&widget, cfg, &state);
    videoHalf.moveToThread(&threadA);

    QObject::connect(&threadA,   &QThread::started,
                     &videoHalf, &VideoHalf::run);
    QObject::connect(&videoHalf, &VideoHalf::paintLeft,
                     &widget,    &MainWidget::onPaintLeft,
                     Qt::QueuedConnection);    QObject::connect(&videoHalf, &VideoHalf::finished,
                     &threadA,   &QThread::quit);
    QObject::connect(&videoHalf, &VideoHalf::finished,
                     &app,       &QApplication::quit,
                     Qt::QueuedConnection);

    /* --- Thread B: bouncing circles (skipped when --circles=0) --- */
    QThread     threadB;
    CircleHalf  circleHalf(&widget, cfg, &state);

    if (cfg.circles > 0) {
        circleHalf.moveToThread(&threadB);
        QObject::connect(&threadB,    &QThread::started,
                         &circleHalf, &CircleHalf::run);
        QObject::connect(&circleHalf, &CircleHalf::paintRight,
                         &widget,     &MainWidget::onPaintRight,
                         Qt::QueuedConnection);
        QObject::connect(&circleHalf, &CircleHalf::finished,
                         &threadB,    &QThread::quit);
    }

    threadA.start();
    if (cfg.circles > 0)
        threadB.start();

    int rc = app.exec();

    /* Signal Thread B to stop if Thread A finished first. */
    state.done = true;
    if (cfg.circles > 0)
        threadB.wait(2000);
    threadA.wait(2000);

    printf("video-frames=%d  circle-frames=%d\n",
           state.videoFrame, state.circleFrame);

    return rc;
}

#include "test_kms_thread.moc"
