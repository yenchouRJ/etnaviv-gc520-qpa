/*
 * test_alpha_blend.cpp — Step 3 verification: overlay ARGB8888 + SRC_OVER blending.
 *
 * Runs both planes in a SINGLE PROCESS / single QApplication:
 *   - The QPA plugin (etnaviv2d, plane=primary) creates the primary screen via
 *     the normal Qt integration path, opening the DRM master fd.
 *   - After Qt is initialized, this test directly constructs a second
 *     QEtnaviv2dScreen for the overlay plane, sharing the primary's fd
 *     (same process = same master; no second open() needed).
 *   - OverlayDriver renders into the overlay BO via QPainter + CPU memcpy
 *     and calls pageFlip() each frame independently of the primary.
 *
 * Visual pass criteria:
 *   T1 (top-left, 80×80): α=0 — fully transparent; primary shows through, no tint.
 *   T2 (below T1):        α=128 red — 50% premultiplied red blended over primary.
 *   T3 (below T2):        α=255 white — fully opaque; primary completely blocked.
 *   HUD: animated semi-transparent (α=160) cyan box bouncing across the image.
 *
 * Build:
 *   (from compositor/) make test_alpha_blend
 *
 * Run on target:
 *   QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=primary \
 *       ./test_alpha_blend [--image=./images/earth.png] [--frames=N] [--verbose]
 */

#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPixmap>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QFileInfo>
#include <QScreen>

#include <cmath>
#include <cstring>

#include "qetnaviv2dscreen.h"

extern "C" {
#include "etna2d.h"
}

/* =========================================================================
 * PrimaryWidget — static background rendered via normal Qt paint path
 * ========================================================================= */
class PrimaryWidget : public QWidget
{
public:
    explicit PrimaryWidget(const QString &imagePath)
    {
        setWindowTitle(QStringLiteral("test_alpha_blend [primary]"));
        setAttribute(Qt::WA_OpaquePaintEvent);

        if (!imagePath.isEmpty()) {
            QImage img;
            if (img.load(imagePath)) {
                img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                m_bg = QPixmap::fromImage(img);
                qDebug() << "primary: loaded" << imagePath
                         << m_bg.width() << "x" << m_bg.height();
            } else {
                qWarning() << "primary: failed to load" << imagePath;
            }
        }

        /* Drive pageFlip at ~60 fps.  Without this Qt only paints once at
         * show() and never calls flush() again — the primary plane goes dark. */
        auto *t = new QTimer(this);
        t->setInterval(16);
        connect(t, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
        t->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!m_bg.isNull()) {
            QSize s = m_bg.size().scaled(size(), Qt::KeepAspectRatio);
            QRect dst((width() - s.width()) / 2, (height() - s.height()) / 2,
                      s.width(), s.height());
            p.drawPixmap(dst, m_bg, m_bg.rect());
        }
    }

private:
    QPixmap m_bg;
};

/* =========================================================================
 * OverlayDriver — renders into the overlay BO each frame and flips
 * ========================================================================= */
class OverlayDriver : public QObject
{
    Q_OBJECT
public:
    OverlayDriver(QEtnaviv2dScreen *screen, bool verbose, int maxFrames,
                  QObject *parent = nullptr)
        : QObject(parent)
        , m_screen(screen)
        , m_verbose(verbose)
        , m_maxFrames(maxFrames)
        , m_W(screen->geometry().width())
        , m_H(screen->geometry().height())
    {
        m_elapsed.start();
        auto *t = new QTimer(this);
        t->setInterval(16);
        connect(t, &QTimer::timeout, this, &OverlayDriver::tick);
        t->start();
    }

private slots:
    void tick()
    {
        renderFrame();
        m_screen->pageFlip();
        ++m_frame;

        if (m_verbose && (m_frame % 30 == 0))
            printVerbose();

        if (m_maxFrames > 0 && m_frame >= m_maxFrames) {
            qint64 ms = m_elapsed.elapsed();
            qDebug().noquote()
                << QStringLiteral("overlay done: frames=%1  elapsed=%2ms  fps=%3")
                   .arg(m_frame).arg(ms)
                   .arg(QString::number(m_frame * 1000.0 / ms, 'f', 1));
            QApplication::quit();
        }
    }

private:
    /* Paint one frame into a QImage, then memcpy to the write-combine BO.
     * CompositionMode_Source is used for the patch fills so the alpha channel
     * is written exactly as specified — not composited over the previous frame. */
    void renderFrame()
    {
        etna2d_surface *surf = m_screen->backSurface();
        if (!surf) return;

        const int W = m_W, H = m_H;
        const double t = m_elapsed.elapsed() / 1000.0;

        QImage img(W, H, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        QPainter p(&img);
        p.setRenderHint(QPainter::TextAntialiasing);

        /* Reference patches — stacked top-left */
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
            p.drawText(QRect(x, y + PS + 2, PS + 60, 18),
                       Qt::AlignLeft | Qt::AlignVCenter, desc);
        };

        drawPatch(PX, PY,
                  QColor(0, 0, 0, 0),
                  QStringLiteral("T1"),
                  QStringLiteral("α=0  transparent"),
                  QColor(200, 200, 200, 180));

        drawPatch(PX, PY + PS + GAP + 16,
                  QColor(128, 0, 0, 128),  /* premultiplied 50% red */
                  QStringLiteral("T2"),
                  QStringLiteral("α=128  50% red"),
                  Qt::white);

        drawPatch(PX, PY + 2 * (PS + GAP) + 32,
                  QColor(255, 255, 255, 255),
                  QStringLiteral("T3"),
                  QStringLiteral("α=255  opaque"),
                  Qt::black);

        /* Animated HUD */
        const int HW = 240, HH = 90;
        int hudX = PX + PS + 80
                   + (int)((W - PX - PS - 100 - HW) * 0.5 * (1.0 + std::sin(t * 0.9)));
        int hudY = qMax((int)((H - HH - 40) * 0.5 * (1.0 + std::cos(t * 0.7))), 10);

        QRect hud(hudX, hudY, HW, HH);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(hud, QColor(6, 13, 38, 160));
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(QPen(QColor(0, 230, 255, 220), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(hud.adjusted(1, 1, -1, -1));
        int barW = (int)(HW * 0.8 * 0.5 * (1.0 + std::sin(t * 2.1)));
        p.fillRect(QRect(hudX + 10, hudY + HH - 20, barW, 10),
                   QColor(0, 230, 255, 180));
        p.setFont(QFont(QStringLiteral("monospace"), 9));
        p.setPen(QColor(0, 230, 255, 255));
        double fps = m_frame > 0 ? m_frame * 1000.0 / m_elapsed.elapsed() : 0;
        p.drawText(hud.adjusted(8, 6, -8, -20), Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("HUD  SRC_OVER  α=160\nframe=%1  fps=%2")
                   .arg(m_frame).arg(QString::number(fps, 'f', 1)));

        p.end();

        /* Copy to write-combine BO. */
        void *ptr = etna2d_surface_map(surf);
        if (ptr) {
            uint8_t *dst = static_cast<uint8_t *>(ptr);
            const uchar *src = img.constBits();
            const int stride = W * 4;
            for (int row = 0; row < H; ++row)
                memcpy(dst + row * stride, src + row * img.bytesPerLine(), stride);
            etna2d_surface_cpu_fini(surf);
        }
    }

    void printVerbose()
    {
        double fps = m_frame > 0 ? m_frame * 1000.0 / m_elapsed.elapsed() : 0;
        qDebug().noquote()
            << QStringLiteral(
               "frame %1  fps=%2\n"
               "  T1  α=0   transparent  → primary pixel shows through, no tint\n"
               "  T2  α=128 50%% red      → red blended over primary\n"
               "  T3  α=255 opaque white → primary completely blocked\n"
               "  HUD α=160 cyan         → semi-transparent over primary")
               .arg(m_frame).arg(QString::number(fps, 'f', 1));
    }

    QEtnaviv2dScreen *m_screen;
    QElapsedTimer     m_elapsed;
    int               m_frame     = 0;
    bool              m_verbose;
    int               m_maxFrames;
    int               m_W, m_H;
};

/* =========================================================================
 * main
 * ========================================================================= */
#include "test_alpha_blend.moc"

int main(int argc, char **argv)
{
    QString imagePath = QStringLiteral("./images/earth.png");
    bool    verbose   = false;
    int     maxFrames = 0;

    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1String("--image=")))
            imagePath = arg.mid(8);
        else if (arg == QLatin1String("--verbose") || arg == QLatin1String("-v"))
            verbose = true;
        else if (arg.startsWith(QLatin1String("--frames=")))
            maxFrames = arg.mid(9).toInt();
        else if (arg == QLatin1String("--help")) {
            qInfo().noquote() << QStringLiteral(
                "Usage: QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=primary \\\n"
                "           ./test_alpha_blend [--image=PATH] [--frames=N] [--verbose]\n"
                "\n"
                "  --image=PATH    background PNG (default: ./images/earth.png)\n"
                "  --frames=N      stop after N overlay frames (default: 0=infinite)\n"
                "  --verbose / -v  print expected patch values every 30 frames\n"
                "\n"
                "Both planes run in a single process.  Set QT_ETNAVIV2D_PLANE=primary\n"
                "(or omit) — the overlay plane is driven directly by this test.");
            return 0;
        }
    }

    QApplication app(argc, argv);

    qDebug().noquote()
        << QStringLiteral("platform=%1  image=%2  frames=%3  verbose=%4")
           .arg(app.platformName()).arg(imagePath)
           .arg(maxFrames > 0 ? QString::number(maxFrames) : QStringLiteral("inf"))
           .arg(verbose ? "yes" : "no");

    PrimaryWidget primary(imagePath);
    primary.showFullScreen();

    /* Obtain the primary screen's DRM fd and GPU context to share with overlay. */
    auto *primaryScreen = static_cast<QEtnaviv2dScreen *>(
                              QGuiApplication::primaryScreen()->handle());
    if (!primaryScreen || primaryScreen->drmFd() < 0) {
        qCritical() << "primary screen is not QEtnaviv2dScreen or has no DRM fd";
        return 1;
    }

    /* Overlay screen: shares primary's fd — same process = same DRM master. */
    auto *overlayScreen = new QEtnaviv2dScreen(
                              QStringLiteral("/dev/dri/card0"),
                              PlaneMode::Overlay,
                              primaryScreen->etnaCtx(),
                              primaryScreen->etnadev(),
                              primaryScreen->drmFd());
    if (!overlayScreen->initialize()) {
        qCritical() << "overlay screen initialize() failed";
        delete overlayScreen;
        return 1;
    }
    qDebug() << "overlay screen ready:" << overlayScreen->geometry();

    OverlayDriver driver(overlayScreen, verbose, maxFrames);
    int ret = app.exec();

    delete overlayScreen;
    return ret;
}
