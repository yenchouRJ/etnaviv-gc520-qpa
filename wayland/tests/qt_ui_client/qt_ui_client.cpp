/*
 * qt_ui_client.cpp -- Milestone B + I validation app.
 *
 * Minimal Qt app that exercises the etnaviv2d QPA in plane=wayland mode.
 * Updated for Milestone I: adds a touch/click counter button so Gate I
 * (input working) is visually verifiable without serial logging.
 *
 * Run on target (Weston must be running):
 *   WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=etnaviv2d:plane=wayland \
 *       QT_ETNAVIV2D_DEBUG=1 ./qt_ui_client [--no-gradient]
 *
 * --no-gradient   Skip the opaque animated background gradient.
 *                 The window is painted with a large semi-transparent T3
 *                 (alpha=200) patch covering most of the surface so Weston's
 *                 plane-promotion policy can be observed on a non-opaque
 *                 client surface.
 *
 * Milestone B gate:
 *   - Qt UI is visible on screen.
 *   - Frames are delivered via dmabuf (not SHM).
 *   - Check: /sys/kernel/debug/dri/0/state — record whether ARGB surface
 *     is on a hardware plane (expected) or in the compositor/renderer.
 *     GL/software fallback is observed here but only gated at Milestone C.
 *
 * The app draws a simple animated colour-cycling gradient + a frame counter
 * so it is obvious whether Qt is rendering live frames.
 * 
 * Gate I check:
 *   Touch / click the blue "Tap me!" button in the lower-right corner.
 *   The counter increments on screen -- this proves wl_touch / wl_pointer
 *   events are reaching Qt widgets through the wl_seat bridge.
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QLabel>
#include <QDateTime>

/* -----------------------------------------------------------------------
 * AnimWidget -- fullscreen widget with animated gradient + Gate I tap counter
 * ----------------------------------------------------------------------- */
class AnimWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AnimWidget(bool noGradient, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_frame(0)
        , m_hue(0)
        , m_noGradient(noGradient)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, !m_noGradient);
        setWindowTitle(QStringLiteral("etnaviv2d wayland milestone-I"));

        /* ---- Gate I: touch/click counter button --------------------------------
         * Large button covering the entire lower half of the screen so it is
         * easy to hit regardless of calibration offset. Tap and lift to increment. */
        m_tapBtn = new QPushButton(QStringLiteral("Tap me!\nCount: 0"), this);
        m_tapBtn->setGeometry(600, 380, 380, 180); /* bottom-right, calibration check */
        QFont bf;
        bf.setPointSize(20);
        bf.setBold(true);
        m_tapBtn->setFont(bf);
        m_tapBtn->setStyleSheet(
            QStringLiteral("QPushButton {"
                           "  background-color: rgba(30,120,200,200);"
                           "  color: white;"
                           "  border: 4px solid white;"
                           "  border-radius: 16px;"
                           "}"
                           "QPushButton:pressed {"
                           "  background-color: rgba(200,80,30,220);"
                           "}"));
        connect(m_tapBtn, &QPushButton::pressed, this, [this]() {
            ++m_tapCount;
            m_tapBtn->setText(
                QStringLiteral("Tap me!\nCount: %1").arg(m_tapCount));
        });

        QTimer *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this]() {
            m_hue = (m_hue + 2) % 360;
            ++m_frame;
            update();
        });
        t->start(33); /* ~30 fps */
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = rect();

        if (m_noGradient) {
            /* --no-gradient mode: clear to transparent, then draw a large
             * semi-transparent T3 patch (alpha=200) covering most of the
             * surface.  This lets us observe whether Weston promotes a
             * non-opaque client surface to a hardware plane. */
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(r, Qt::transparent);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            /* Large alpha patch — covers ~80% of the surface */
            p.fillRect(r.adjusted(40, 40, -40, -40), QColor(180, 60, 60, 200));
        } else {
            /* Background: animated hue gradient (opaque) */
            QLinearGradient grad(r.topLeft(), r.bottomRight());
            grad.setColorAt(0.0, QColor::fromHsv(m_hue,          200, 220));
            grad.setColorAt(0.5, QColor::fromHsv((m_hue + 90) % 360, 200, 180));
            grad.setColorAt(1.0, QColor::fromHsv((m_hue + 180) % 360, 200, 220));
            p.fillRect(r, grad);
        }

        /* Alpha blend test patches (mirrors T1/T2/T3 from test_composite) */
        /* T1: alpha=0 (transparent) */
        p.fillRect(QRect(40, 40, 120, 80),  QColor(255, 0, 0, 0));
        /* T2: alpha=128 (50% blend) */
        p.fillRect(QRect(40, 140, 120, 80), QColor(255, 0, 0, 128));
        /* T3: alpha=255 (opaque) — or alpha=200 large patch in no-gradient mode */
        if (!m_noGradient)
            p.fillRect(QRect(40, 240, 120, 80), QColor(255, 0, 0, 255));

        /* Frame counter + timestamp */
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSize(18);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(200, 40, r.width() - 240, 60),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("etnaviv2d QPA -- Wayland (Milestone I: input)"));
        f.setPointSize(14);
        f.setBold(false);
        p.setFont(f);
        p.drawText(QRect(200, 110, r.width() - 240, 40),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("frame=%1  hue=%2").arg(m_frame).arg(m_hue));
        p.drawText(QRect(200, 155, r.width() - 240, 40),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QDateTime::currentDateTime().toString(
                       QStringLiteral("hh:mm:ss.zzz")));

        /* Patch labels */
        f.setPointSize(11);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(QRect(40, 120,  120, 20), Qt::AlignCenter, QStringLiteral("T1 α=0"));
        p.drawText(QRect(40, 220, 120, 20), Qt::AlignCenter, QStringLiteral("T2 α=128"));
        if (!m_noGradient)
            p.drawText(QRect(40, 320, 120, 20), Qt::AlignCenter, QStringLiteral("T3 α=255"));
    }

private:
    int         m_frame;
    int         m_hue;
    bool        m_noGradient;
    int         m_tapCount = 0;
    QPushButton *m_tapBtn  = nullptr;
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* Wayland mode is selected automatically when WAYLAND_DISPLAY is set,
     * or explicitly via QT_QPA_PLATFORM=etnaviv2d:plane=wayland.
     *
     * --no-gradient   disable the opaque animated gradient background;
     *                 renders a large semi-transparent T3 (α=200) patch
     *                 instead, useful for observing Weston plane-promotion
     *                 behaviour on a non-opaque surface. */
    bool noGradient = false;
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--no-gradient") {
            noGradient = true;
            break;
        }
    }

    QApplication app(argc, argv);

    AnimWidget w(noGradient);
    w.showFullScreen();

    return app.exec();
}

#include "qt_ui_client.moc"
