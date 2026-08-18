/*
 * test_composite.cpp — Step 4: single-committer dual-plane video+UI
 * (plane=composite).
 *
 * Architecture (D13/D14/D15/D16/D18):
 *   - QApplication with QT_ETNAVIV2D_PLANE=composite.
 *   - HudWidget: full-screen translucent widget on the overlay plane.  Driven
 *     via the normal QPA backing store path (D16).  Paints three reference
 *     blend patches (T1/T2/T3, same as test_alpha_blend) plus an animated
 *     bouncing cyan HUD box, all over the live NV12 video on the primary.
 *   - VideoThread: owns the 3× NV12 BO ring (one atomic on_screen bit per
 *     buffer, D9b).  Reads frames from a pre-decoded raw NV12 file in a loop
 *     and calls screen->attachPrimaryFb() each frame.  The screen folds the
 *     staged FB into its single atomic commit (D3).
 *   - main: resolves the composite screen, starts both actors.
 *
 * Pass criteria (Step 4 — full blend over video):
 *   T1  α=0   — fully transparent patch; live video shows through with no tint.
 *   T2  α=128 — 50% premultiplied red blended over moving video.
 *   T3  α=255 — opaque white patch; video completely blocked.
 *   HUD  α=160 animated cyan box bouncing across the video; smooth SRC_OVER.
 *   No tearing on either plane (single commit → single vblank, D9b).
 *   Video keeps advancing when UI is static after --static-after=N (D12):
 *     ui= counter freezes, vid= keeps climbing.
 *
 * Build:
 *   (from compositor/) make test_composite
 *
 * Run on target:
 *   QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=composite \
 *       ./test_composite \
 *           --raw-video=./images/earth_nv12_1024x600.raw \
 *           --video-size=1024x600 \
 *           [--video-fps=30] [--static-after=N] [--frames=N] [--verbose]
 */

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QFont>
#include <QScreen>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <QDebug>
#include <QFile>
#include <QSize>
#include <QCoreApplication>
#include <cstring>
#include <atomic>
#include <cerrno>
#include <cmath>

#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "qetnaviv2dscreen.h"

/* =========================================================================
 * HudWidget — full-screen translucent overlay driven via backing store (D16)
 *
 * Paints:
 *   T1 (top-left)  α=0   transparent — primary video shows through
 *   T2 (below T1)  α=128 premultiplied 50% red — blended over video
 *   T3 (below T2)  α=255 opaque white — video completely blocked
 *   HUD             α=160 animated bouncing cyan box, ui= / vid= counters
 * ========================================================================= */
class HudWidget : public QWidget
{
    Q_OBJECT
public:
    explicit HudWidget(int staticAfter, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_staticAfter(staticAfter)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setWindowFlags(Qt::FramelessWindowHint);
        m_elapsed.start();

        m_timer = new QTimer(this);
        m_timer->setInterval(16); /* ~60 fps until --static-after fires */
        connect(m_timer, &QTimer::timeout, this, [this]() {
            ++m_uiFrames;
            if (m_staticAfter > 0 && m_uiFrames >= m_staticAfter) {
                m_timer->stop();
                qDebug() << "[hud] UI animation stopped at frame" << m_uiFrames
                         << "— primary video continues to advance";
            }
            update();
        });
        m_timer->start();
    }

    void incVidFrames() { m_vidFrames.fetch_add(1); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const int W = width(), H = height();
        const double t = m_elapsed.elapsed() / 1000.0;

        /* Completely transparent background — video shows through. */
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);

        /* ---- Reference patches (stacked top-left, same as test_alpha_blend) ---- */
        const int PX = 20, PY = 20, PS = 80, GAP = 20;
        const QFont labelFont(QStringLiteral("monospace"), 11, QFont::Bold);
        const QFont descFont(QStringLiteral("monospace"), 8);

        auto drawPatch = [&](int x, int y, QColor fill,
                             const QString &label, const QString &desc,
                             QColor textCol) {
            QRect r(x, y, PS, PS);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(r, fill);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            p.setPen(QPen(textCol, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(0, 0, -1, -1));
            p.setFont(labelFont);
            p.setPen(textCol);
            p.drawText(r, Qt::AlignCenter, label);
            p.setFont(descFont);
            p.setPen(QColor(220, 220, 220, 200));
            p.drawText(QRect(x, y + PS + 2, PS + 80, 18),
                       Qt::AlignLeft | Qt::AlignVCenter, desc);
        };

        /* T1: α=0 transparent */
        drawPatch(PX, PY,
                  QColor(0, 0, 0, 0),
                  QStringLiteral("T1"),
                  QStringLiteral("α=0  transparent"),
                  QColor(200, 200, 200, 180));

        /* T2: α=128 premultiplied 50% red */
        drawPatch(PX, PY + PS + GAP + 16,
                  QColor(128, 0, 0, 128),
                  QStringLiteral("T2"),
                  QStringLiteral("α=128  50% red"),
                  Qt::white);

        /* T3: α=255 opaque white */
        drawPatch(PX, PY + 2 * (PS + GAP) + 32,
                  QColor(255, 255, 255, 255),
                  QStringLiteral("T3"),
                  QStringLiteral("α=255  opaque"),
                  Qt::black);

        /* ---- Animated HUD box (bouncing) ---- */
        const int HW = 300, HH = 100;
        int hudX = PX + PS + 100
                   + (int)((W - PX - PS - 120 - HW) * 0.5 * (1.0 + std::sin(t * 0.9)));
        int hudY = qMax((int)((H - HH - 40) * 0.5 * (1.0 + std::cos(t * 0.7))), 10);

        QRect hud(hudX, hudY, HW, HH);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(hud, QColor(6, 13, 38, 160));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(QColor(0, 230, 255, 220), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(hud.adjusted(1, 1, -1, -1));

        /* Progress bar inside HUD */
        int barW = (int)(HW * 0.8 * 0.5 * (1.0 + std::sin(t * 2.1)));
        p.fillRect(QRect(hudX + 10, hudY + HH - 22, barW, 10),
                   QColor(0, 230, 255, 180));

        double uiFps = m_uiFrames > 0
                       ? m_uiFrames * 1000.0 / m_elapsed.elapsed()
                       : 0.0;
        p.setFont(QFont(QStringLiteral("monospace"), 9));
        p.setPen(QColor(0, 230, 255, 255));
        p.drawText(hud.adjusted(8, 6, -8, -24),
                   Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("HUD  SRC_OVER  α=160\n"
                                  "ui=%1  fps=%2\n"
                                  "vid=%3")
                   .arg(m_uiFrames)
                   .arg(QString::number(uiFps, 'f', 1))
                   .arg(m_vidFrames.load()));
    }

private:
    QTimer            *m_timer;
    QElapsedTimer      m_elapsed;
    int                m_uiFrames  = 0;
    int                m_staticAfter;
    std::atomic<int>   m_vidFrames{0};
};

/* =========================================================================
 * NV12 BO ring — 3 buffers (D9b)
 *
 * Buffer states tracked by two atomic pointers in VideoThread:
 *   m_staged  : buffer handed to the screen, waiting for next commit
 *   m_onScreen: buffer currently scanned by the display controller
 *
 * pickFree() skips both.  With 3 buffers at most 1 staged + 1 on-screen
 * at any time → always at least 1 free.  No per-buffer state needed.
 * ========================================================================= */
static constexpr int NV12_RING_SIZE = 3;

struct NV12Buf {
    uint32_t  handle = 0;
    uint32_t  fbId   = 0;
    void     *map    = nullptr;
    size_t    size   = 0;
};

/* =========================================================================
 * VideoThread — owns the NV12 BO ring, reads frames, calls attachPrimaryFb
 * ========================================================================= */
class VideoThread : public QThread
{
    Q_OBJECT
public:
    VideoThread(QEtnaviv2dScreen *screen,
                const QString    &rawFile,
                QSize             videoSize,
                int               fps,
                int               maxFrames,
                bool              verbose,
                HudWidget        *hud,
                QObject          *parent = nullptr)
        : QThread(parent)
        , m_screen(screen)
        , m_rawFile(rawFile)
        , m_videoSize(videoSize)
        , m_fps(fps > 0 ? fps : 30)
        , m_maxFrames(maxFrames)
        , m_verbose(verbose)
        , m_hud(hud)
    {}

    ~VideoThread() override
    {
        requestInterruption();
        wait();
        freeBufs();
    }

signals:
    void done();

protected:
    void run() override
    {
        int kfd = m_screen->drmFd();
        int W   = m_videoSize.width();
        int H   = m_videoSize.height();

        /* --- Allocate the 3-buffer NV12 BO ring (D9b). --- */
        if (!allocBufs(kfd, W, H)) {
            qCritical() << "[video] NV12 BO allocation failed";
            emit done();
            return;
        }

        /* --- Register the release callback (D18).
         * Called after the flip-event drain (GUI thread).  The screen passes
         * back info.cookie — the NV12Buf that just left the display controller.
         * Clear m_onScreen so pickFree() can reuse that slot. */
        m_screen->setPrimaryReleaseCallback([this](const PrimaryFbInfo &info) {
            /* Flip event fired: the previously-on-screen buffer is now free.
             * Unconditionally clear m_onScreen — only one buffer can be
             * retired at a time (sequential flip events, single CRTC). */
            m_onScreen.store(nullptr);
            if (m_verbose)
                qDebug() << "[video] released fbId=" << info.fbId;
        });

        /* --- Open the raw NV12 file. --- */
        QFile f(m_rawFile);
        if (!f.open(QIODevice::ReadOnly)) {
            qCritical() << "[video] cannot open" << m_rawFile;
            cleanup(kfd);
            emit done();
            return;
        }

        const qint64 frameBytes    = (qint64)W * H * 3 / 2;
        const int    totalFrames   = (int)(f.size() / frameBytes);
        qDebug().noquote()
            << QStringLiteral("[video] %1x%2  %3 bytes/frame  %4 frames  fps=%5")
               .arg(W).arg(H).arg(frameBytes).arg(totalFrames).arg(m_fps);

        const int     intervalMs = 1000 / m_fps;
        QElapsedTimer clock;
        clock.start();
        int loopCount = 0, frame = 0;

        while (!isInterruptionRequested()) {
            /* Pick a free buffer: not on_screen AND not the currently staged
             * one.  With 3 buffers there is always at least one free as long
             * as at most one is on_screen and one is staged. */
            NV12Buf *buf = pickFree();
            if (!buf) {
                QThread::usleep(500);
                continue;
            }

            /* Loop back to start after last frame. */
            int fileFrame = frame % totalFrames;
            if (fileFrame == 0 && frame > 0) {
                ++loopCount;
                if (m_verbose)
                    qDebug() << "[video] loop" << loopCount;
            }
            f.seek((qint64)fileFrame * frameBytes);
            qint64 n = f.read(static_cast<char *>(buf->map), frameBytes);
            if (n != frameBytes) {
                qWarning() << "[video] short read at frame" << frame;
                continue;
            }

            /* Advance the ring state:
             *   m_onScreen ← previous m_staged (it's about to be committed)
             *   m_staged   ← buf (the new staging candidate)
             * pickFree() skips both, so 2 buffers are "consumed" and 1 is
             * always available. */
            m_onScreen.store(m_staged.load());
            m_staged.store(buf);

            PrimaryFbInfo info;
            info.fbId   = buf->fbId;
            info.cookie = buf;
            m_screen->attachPrimaryFb(info);

            if (m_hud)
                m_hud->incVidFrames();

            ++frame;

            if (m_verbose && (frame % (m_fps * 5) == 0))
                qDebug() << "[video] frame=" << frame << "loop=" << loopCount;

            if (m_maxFrames > 0 && frame >= m_maxFrames) {
                qDebug().noquote()
                    << QStringLiteral("[video] done: frames=%1  loops=%2")
                       .arg(frame).arg(loopCount);
                break;
            }

            /* Pace to --video-fps: sleep until next frame deadline.
             * Cap sleep to one frame interval to avoid stalling if the clock
             * jumped (e.g. debugger pause). */
            qint64 sleepMs = (qint64)frame * intervalMs - clock.elapsed();
            if (sleepMs > 1 && sleepMs <= intervalMs)
                QThread::msleep((unsigned long)sleepMs);
        }

        f.close();
        /* Let the screen retire the last on-screen buffer before freeing BOs. */
        QThread::msleep(100);
        cleanup(kfd);
        emit done();
    }

private:
    /* Allocate NV12 dumb BOs: each BO is W × (H*3/2) @ 8bpp.
     * The Y plane occupies the first W*H bytes; the interleaved UV plane
     * follows at offset W*H.  drmModeAddFB2 is called with two virtual
     * planes pointing at the same BO handle. */
    bool allocBufs(int kfd, int W, int H)
    {
        for (int i = 0; i < NV12_RING_SIZE; ++i) {
            NV12Buf &b = m_bufs[i];

            struct drm_mode_create_dumb creq = {};
            creq.width  = W;
            creq.height = H * 3 / 2;
            creq.bpp    = 8;
            if (drmIoctl(kfd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
                qWarning() << "[video] CREATE_DUMB[" << i << "]:" << strerror(errno);
                return false;
            }
            b.handle = creq.handle;
            b.size   = creq.size;

            struct drm_mode_map_dumb mreq = {};
            mreq.handle = b.handle;
            if (drmIoctl(kfd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
                qWarning() << "[video] MAP_DUMB[" << i << "]:" << strerror(errno);
                return false;
            }
            b.map = mmap(nullptr, b.size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, kfd, mreq.offset);
            if (b.map == MAP_FAILED) {
                b.map = nullptr;
                qWarning() << "[video] mmap[" << i << "]:" << strerror(errno);
                return false;
            }

            uint32_t handles[4] = { b.handle, b.handle, 0, 0 };
            uint32_t pitches[4] = { (uint32_t)W, (uint32_t)W, 0, 0 };
            uint32_t offsets[4] = { 0, (uint32_t)(W * H), 0, 0 };
            if (drmModeAddFB2(kfd, W, H, DRM_FORMAT_NV12,
                              handles, pitches, offsets, &b.fbId, 0)) {
                qWarning() << "[video] drmModeAddFB2[" << i << "]:" << strerror(errno);
                return false;
            }
            qDebug() << "[video] NV12 buf[" << i << "]"
                     << "fbId=" << b.fbId
                     << "handle=" << b.handle
                     << "size=" << b.size;
        }
        return true;
    }

    void freeBufs()
    {
        for (int i = 0; i < NV12_RING_SIZE; ++i) {
            if (m_bufs[i].map && m_bufs[i].map != MAP_FAILED) {
                munmap(m_bufs[i].map, m_bufs[i].size);
                m_bufs[i].map = nullptr;
            }
        }
    }

    void cleanup(int kfd)
    {
        for (int i = 0; i < NV12_RING_SIZE; ++i) {
            if (m_bufs[i].fbId) {
                drmModeRmFB(kfd, m_bufs[i].fbId);
                m_bufs[i].fbId = 0;
            }
            if (m_bufs[i].handle) {
                struct drm_mode_destroy_dumb dreq = { .handle = m_bufs[i].handle };
                drmIoctl(kfd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
                m_bufs[i].handle = 0;
            }
        }
    }

    NV12Buf *pickFree()
    {
        NV12Buf *staged   = m_staged.load();
        NV12Buf *onScreen = m_onScreen.load();
        for (int i = 0; i < NV12_RING_SIZE; ++i) {
            if (&m_bufs[i] != staged && &m_bufs[i] != onScreen)
                return &m_bufs[i];
        }
        return nullptr;
    }

    QEtnaviv2dScreen             *m_screen;
    QString                       m_rawFile;
    QSize                         m_videoSize;
    int                           m_fps;
    int                           m_maxFrames;
    bool                          m_verbose;
    HudWidget                    *m_hud;
    NV12Buf                       m_bufs[NV12_RING_SIZE];
    std::atomic<NV12Buf *>        m_staged{nullptr};
    std::atomic<NV12Buf *>        m_onScreen{nullptr};
};

/* =========================================================================
 * main
 * ========================================================================= */
#include "test_composite.moc"

int main(int argc, char **argv)
{
    QString rawVideo;
    QSize   videoSize(1024, 600);
    int     videoFps    = 30;
    int     staticAfter = 0;
    int     maxFrames   = 0;
    bool    verbose     = false;

    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1String("--raw-video=")))
            rawVideo = arg.mid(12);
        else if (arg.startsWith(QLatin1String("--video-size="))) {
            QStringList parts = arg.mid(13).split(QLatin1Char('x'));
            if (parts.size() == 2)
                videoSize = QSize(parts[0].toInt(), parts[1].toInt());
        }
        else if (arg.startsWith(QLatin1String("--video-fps=")))
            videoFps = arg.mid(12).toInt();
        else if (arg.startsWith(QLatin1String("--static-after=")))
            staticAfter = arg.mid(15).toInt();
        else if (arg.startsWith(QLatin1String("--frames=")))
            maxFrames = arg.mid(9).toInt();
        else if (arg == QLatin1String("--verbose") || arg == QLatin1String("-v"))
            verbose = true;
        else if (arg == QLatin1String("--help")) {
            qInfo().noquote() << QStringLiteral(
                "Usage: QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=composite \\\n"
                "           ./test_composite \\\n"
                "               --raw-video=FILE --video-size=WxH \\\n"
                "               [--video-fps=N] [--static-after=N] [--frames=N] [--verbose]\n"
                "\n"
                "  --raw-video=FILE      pre-decoded raw NV12 file (mandatory)\n"
                "  --video-size=WxH      frame dimensions (default 1024x600)\n"
                "  --video-fps=N         playback rate (default 30)\n"
                "  --static-after=N      stop UI animation after N frames (0=never)\n"
                "  --frames=N            quit after N video frames (0=infinite)\n"
                "  --verbose / -v        verbose video-thread output every 5 s\n"
                "\n"
                "Visual pass criteria:\n"
                "  T1  α=0   top-left patch: fully transparent, video shows through\n"
                "  T2  α=128 50%% premultiplied red blended over moving video\n"
                "  T3  α=255 opaque white, video completely blocked\n"
                "  HUD α=160 animated bouncing cyan box, smooth SRC_OVER\n"
                "  No tearing on primary (single atomic commit → single vblank)\n"
                "  vid= counter advances while ui= is frozen (--static-after=N)");
            return 0;
        }
    }

    if (rawVideo.isEmpty()) {
        qCritical() << "error: --raw-video=FILE is required\n"
                       "       example: --raw-video=./images/earth_nv12_1024x600.raw";
        return 1;
    }

    QApplication app(argc, argv);

    qDebug().noquote()
        << QStringLiteral("platform=%1  raw-video=%2  size=%3x%4  fps=%5  "
                          "static-after=%6  frames=%7")
           .arg(app.platformName())
           .arg(rawVideo)
           .arg(videoSize.width()).arg(videoSize.height())
           .arg(videoFps)
           .arg(staticAfter > 0 ? QString::number(staticAfter) : QStringLiteral("never"))
           .arg(maxFrames > 0 ? QString::number(maxFrames) : QStringLiteral("inf"));

    /* Resolve the composite screen via the normal Qt integration path (D16). */
    auto *screen = static_cast<QEtnaviv2dScreen *>(
                       QGuiApplication::primaryScreen()->handle());
    if (!screen || screen->drmFd() < 0) {
        qCritical() << "primary screen is not QEtnaviv2dScreen or has no DRM fd";
        return 1;
    }

    /* HUD widget — full-screen, translucent, driven via backing store (D16).
     * Paints T1/T2/T3 reference patches + animated bouncing HUD box over the
     * NV12 video on the primary plane. */
    HudWidget hud(staticAfter);
    hud.showFullScreen();

    /* Video thread — owns the NV12 BO ring and calls attachPrimaryFb(). */
    VideoThread vt(screen, rawVideo, videoSize,
                   videoFps, maxFrames, verbose, &hud);
    QObject::connect(&vt, &VideoThread::done, &app, &QApplication::quit);
    vt.start();

    int ret = app.exec();
    vt.requestInterruption();
    vt.wait();
    return ret;
}
