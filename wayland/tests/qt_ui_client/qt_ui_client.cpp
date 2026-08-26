/*
 * qt_ui_client.cpp — Milestone B validation app.
 *
 * Minimal Qt app that exercises the etnaviv2d QPA in plane=wayland mode.
 * The QPA connects to Weston as a Wayland client, exports an ARGB dmabuf
 * ring, and drives frames via linux-dmabuf-v1 + xdg_toplevel.
 *
 * Run on target (Weston must be running):
 *   WAYLAND_DISPLAY=wayland-1 QT_QPA_PLATFORM=etnaviv2d:plane=wayland \
 *       QT_ETNAVIV2D_DEBUG=1 ./qt_ui_client
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
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QTimer>
#include <QLabel>
#include <QVBoxLayout>
#include <QDateTime>

/* -----------------------------------------------------------------------
 * AnimWidget — fullscreen widget with animated colour cycling + frame info
 * ----------------------------------------------------------------------- */
class AnimWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AnimWidget(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_frame(0)
        , m_hue(0)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setWindowTitle(QStringLiteral("etnaviv2d wayland milestone-B"));

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

        /* Background: animated hue gradient */
        QLinearGradient grad(r.topLeft(), r.bottomRight());
        grad.setColorAt(0.0, QColor::fromHsv(m_hue,          200, 220));
        grad.setColorAt(0.5, QColor::fromHsv((m_hue + 90) % 360, 200, 180));
        grad.setColorAt(1.0, QColor::fromHsv((m_hue + 180) % 360, 200, 220));
        p.fillRect(r, grad);

        /* Alpha blend test patches (mirrors T1/T2/T3 from test_composite) */
        /* T1: alpha=0 (transparent) */
        p.fillRect(QRect(40, 40, 120, 80),  QColor(255, 0, 0, 0));
        /* T2: alpha=128 (50% blend) */
        p.fillRect(QRect(40, 140, 120, 80), QColor(255, 0, 0, 128));
        /* T3: alpha=255 (opaque) */
        p.fillRect(QRect(40, 240, 120, 80), QColor(255, 0, 0, 255));

        /* Frame counter + timestamp */
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPointSize(18);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(200, 40, r.width() - 240, 60),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("etnaviv2d QPA — Wayland mode (Milestone B)"));
        f.setPointSize(14);
        f.setBold(false);
        p.setFont(f);
        p.drawText(QRect(200, 110, r.width() - 240, 40),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("frame=%1  hue=%2")
                   .arg(m_frame).arg(m_hue));
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
        p.drawText(QRect(40, 320, 120, 20), Qt::AlignCenter, QStringLiteral("T3 α=255"));
    }

private:
    int m_frame;
    int m_hue;
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    /* Wayland mode is selected automatically when WAYLAND_DISPLAY is set,
     * or explicitly via QT_QPA_PLATFORM=etnaviv2d:plane=wayland */
    QApplication app(argc, argv);

    AnimWidget w;
    w.showFullScreen();

    return app.exec();
}

#include "qt_ui_client.moc"
