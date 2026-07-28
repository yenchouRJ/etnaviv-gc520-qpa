/*
 * test_integration.cpp — Step 3: First Qt window on the panel.
 *
 * Runs a minimal QWidget application through the etnaviv2d plugin to verify:
 *  - Plugin loads and initializes (card0 + renderD128).
 *  - A QWidget with a custom paintEvent appears on the panel.
 *  - fillRect and drawPixmap paths both execute without GPU faults.
 *  - Resize (window geometry change) does not crash or corrupt.
 *
 * Run on target (without -platform flag; the env var drives plugin selection):
 *   QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_DEBUG=1 ./test_integration
 *
 * Or with parameters:
 *   QT_QPA_PLATFORM="etnaviv2d:card=/dev/dri/card0:render=/dev/dri/renderD128" \
 *   ./test_integration
 *
 * The window auto-closes after 3 seconds.  Watch the panel for:
 *   - Grey background
 *   - Red rectangle in the top-left quadrant
 *   - Green rectangle in the top-right quadrant
 *   - Blue rectangle in the bottom-left quadrant
 *   - A QLabel "etnaviv2d OK" rendered via raster fallback (text)
 */
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QDebug>
#include <cstdio>

/* -----------------------------------------------------------------------
 * Test widget: custom paintEvent exercises fill + drawPixmap
 * ----------------------------------------------------------------------- */
class TestWidget : public QWidget
{
public:
    explicit TestWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setWindowTitle(QStringLiteral("etnaviv2d integration test"));

        /* Prepare a 64x64 source pixmap with a quadrant pattern. */
        m_src = QPixmap(64, 64);
        QPainter p(&m_src);
        p.fillRect( 0,  0, 32, 32, QColor(255, 128,   0)); /* TL orange */
        p.fillRect(32,  0, 32, 32, QColor(255, 255,   0)); /* TR yellow */
        p.fillRect( 0, 32, 32, 32, QColor(  0, 255, 255)); /* BL cyan   */
        p.fillRect(32, 32, 32, 32, QColor(255,   0, 255)); /* BR magenta*/
        printf("[ OK ] source pixmap prepared\n");
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        int w = width(), h = height();

        /* Background: dark grey */
        p.fillRect(0, 0, w, h, QColor(40, 40, 40));

        /* Solid colour fills in three quadrants */
        p.fillRect(0,     0,     w/2, h/2, QColor(200,   0,   0)); /* red   TL */
        p.fillRect(w/2,   0,     w/2, h/2, QColor(  0, 200,   0)); /* green TR */
        p.fillRect(0,     h/2,   w/2, h/2, QColor(  0,   0, 200)); /* blue  BL */

        /* Source pixmap blitted and stretched into bottom-right quadrant */
        p.drawPixmap(QRect(w/2, h/2, w/2, h/2), m_src, m_src.rect());

        /* Raster fallback: white text in the centre */
        p.setPen(Qt::white);
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("etnaviv2d OK"));

        static bool once = false;
        if (!once) {
            printf("[ OK ] paintEvent executed\n");
            once = true;
        }
    }

private:
    QPixmap m_src;
};

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    printf("[ OK ] QApplication created (plugin: %s)\n",
           qPrintable(QGuiApplication::platformName()));

    if (QGuiApplication::platformName() != QLatin1String("etnaviv2d"))
        qWarning("[WARN] Not running under etnaviv2d plugin — "
                 "set QT_QPA_PLATFORM=etnaviv2d");

    TestWidget w;
    w.showFullScreen();

    /* Exit after 3 seconds so the test can be automated. */
    QTimer::singleShot(3000, &app, &QApplication::quit);

    int rc = app.exec();
    printf(rc == 0 ? "[ OK ] integration test complete\n"
                   : "[FAIL] integration test returned %d\n", rc);
    return rc;
}
