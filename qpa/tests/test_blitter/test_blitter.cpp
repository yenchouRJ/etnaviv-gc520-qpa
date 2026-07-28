/*
 * test_blitter.cpp — Step 1: EtnaBlittable unit test (no display required).
 *
 * Tests the blitter layer in isolation: creates a QPixmap backed by an
 * EtnaBlittablePlatformPixmap, exercises GPU ops via QPainter, then reads
 * back via toImage() and compares pixel values to a CPU reference.
 *
 * Run on the target:
 *   QT_ETNAVIV2D_DEBUG=1 ./test_blitter [/dev/dri/renderD128]
 *
 * Expected output: all tests PASS, pixel values printed for verification.
 * A mismatch means the blitter emitted wrong ops or the surface format/
 * stride is wrong.
 *
 * This test does NOT open a display (card0).  It only needs renderD128.
 */
#include <QGuiApplication>
#include <QPixmap>
#include <QPainter>
#include <QImage>
#include <QDebug>
#include <cstdio>

extern "C" {
#include "etna2d.h"
}

/* The blitter uses a global context set by setGlobalContext(); replicate
 * that setup here without the full integration. */
#include "../../src/qetnaviv2dblitter.h"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static int gFailed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        ++gFailed; \
    } else { \
        printf("[ OK ] %s\n", msg); \
    } \
} while (0)

/* Tolerance for premultiplied round-trip: ±2 per channel */
static bool pixelClose(QRgb a, QRgb b, int tol = 2)
{
    return qAbs(qRed(a)   - qRed(b))   <= tol &&
           qAbs(qGreen(a) - qGreen(b)) <= tol &&
           qAbs(qBlue(a)  - qBlue(b))  <= tol &&
           qAbs(qAlpha(a) - qAlpha(b)) <= tol;
}

/* -----------------------------------------------------------------------
 * Test 1: fillRect — solid opaque fill
 * ----------------------------------------------------------------------- */
static bool testFillRect()
{
    printf("\n--- Test 1: fillRect (solid fill) ---\n");

    QPixmap pm(64, 64);
    QPainter p(&pm);
    p.fillRect(0, 0, 64, 64, QColor(255, 0, 0, 255)); /* opaque red */
    p.end();

    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    QRgb got    = img.pixel(32, 32);
    QRgb expect = qRgba(255, 0, 0, 255);

    printf("  pixel(32,32) = #%08x  expected #%08x\n", got, expect);
    CHECK(pixelClose(got, expect), "fillRect: red pixel at centre");

    /* Fill sub-rect with blue */
    QPainter p2(&pm);
    p2.fillRect(10, 10, 20, 20, QColor(0, 0, 255, 255));
    p2.end();

    img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    QRgb got2 = img.pixel(20, 20);
    QRgb exp2 = qRgba(0, 0, 255, 255);
    printf("  pixel(20,20) = #%08x  expected #%08x\n", got2, exp2);
    CHECK(pixelClose(got2, exp2), "fillRect: blue sub-rect");

    /* Area outside sub-rect still red */
    QRgb got3 = img.pixel(5, 5);
    printf("  pixel(5,5)   = #%08x  expected #%08x\n", got3, expect);
    CHECK(pixelClose(got3, expect), "fillRect: area outside sub-rect unchanged");

    return gFailed == 0;
}

/* -----------------------------------------------------------------------
 * Test 2: drawPixmap — GPU blit between two pixmaps
 * ----------------------------------------------------------------------- */
static bool testDrawPixmap()
{
    printf("\n--- Test 2: drawPixmap (GPU blit) ---\n");
    int prev = gFailed;

    /* Source: 32x32 green pixmap */
    QPixmap src(32, 32);
    {
        QPainter p(&src);
        p.fillRect(src.rect(), QColor(0, 200, 0, 255));
    }

    /* Destination: 64x64 grey background */
    QPixmap dst(64, 64);
    {
        QPainter p(&dst);
        p.fillRect(dst.rect(), QColor(128, 128, 128, 255));
        p.drawPixmap(16, 16, src); /* place src at (16,16) */
    }

    QImage img = dst.toImage().convertToFormat(QImage::Format_ARGB32);

    /* Centre of blitted region: should be green */
    QRgb got1 = img.pixel(32, 32);
    QRgb exp1 = qRgba(0, 200, 0, 255);
    printf("  pixel(32,32) = #%08x  expected #%08x  [inside blit]\n", got1, exp1);
    CHECK(pixelClose(got1, exp1), "drawPixmap: centre of blit region");

    /* Outside blit region: should be grey */
    QRgb got2 = img.pixel(5, 5);
    QRgb exp2 = qRgba(128, 128, 128, 255);
    printf("  pixel(5,5)   = #%08x  expected #%08x  [outside blit]\n", got2, exp2);
    CHECK(pixelClose(got2, exp2), "drawPixmap: outside blit region unchanged");

    return gFailed == prev;
}

/* -----------------------------------------------------------------------
 * Test 3: stretch blit — drawPixmap with different src/dst sizes
 * ----------------------------------------------------------------------- */
static bool testStretchBlit()
{
    printf("\n--- Test 3: stretch blit ---\n");
    int prev = gFailed;

    /* Source: 16x16 with 4 colour quadrants (matching Phase-2 selftest) */
    QPixmap src(16, 16);
    {
        QPainter p(&src);
        p.fillRect( 0,  0, 8, 8, QColor(255,   0,   0, 255)); /* TL red   */
        p.fillRect( 8,  0, 8, 8, QColor(  0, 255,   0, 255)); /* TR green */
        p.fillRect( 0,  8, 8, 8, QColor(  0,   0, 255, 255)); /* BL blue  */
        p.fillRect( 8,  8, 8, 8, QColor(255, 255,   0, 255)); /* BR yell  */
    }

    /* Destination: 64x64; stretch source 2× */
    QPixmap dst(64, 64);
    {
        QPainter p(&dst);
        p.fillRect(dst.rect(), QColor(40, 40, 40, 255));
        p.drawPixmap(QRect(0, 0, 32, 32), src, src.rect());
    }

    QImage img = dst.toImage().convertToFormat(QImage::Format_ARGB32);

    /* Sample each quadrant of the stretched output */
    auto chk = [&](int x, int y, QRgb exp, const char *label) {
        QRgb got = img.pixel(x, y);
        printf("  pixel(%d,%d) = #%08x  expected #%08x  [%s]\n",
               x, y, got, exp, label);
        CHECK(pixelClose(got, exp, 4), label);
    };

    chk( 8,  8, qRgba(255,   0,   0, 255), "stretch TL=red");
    chk(24,  8, qRgba(  0, 255,   0, 255), "stretch TR=green");
    chk( 8, 24, qRgba(  0,   0, 255, 255), "stretch BL=blue");
    chk(24, 24, qRgba(255, 255,   0, 255), "stretch BR=yellow");

    return gFailed == prev;
}

/* -----------------------------------------------------------------------
 * Test 4: raster fallback — draw text (no glyph HW path; raster fallback)
 * ----------------------------------------------------------------------- */
static bool testRasterFallback()
{
    printf("\n--- Test 4: raster fallback (text rendering) ---\n");
    int prev = gFailed;

    QPixmap pm(128, 32);
    {
        QPainter p(&pm);
        p.fillRect(pm.rect(), Qt::white);
        p.setPen(Qt::black);
        p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("Hi"));
    }

    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);

    /* At least one non-white pixel must exist (text rendered). */
    bool hasBlack = false;
    for (int y = 0; y < img.height() && !hasBlack; ++y)
        for (int x = 0; x < img.width() && !hasBlack; ++x)
            if (qRed(img.pixel(x, y)) < 128)
                hasBlack = true;

    CHECK(hasBlack, "raster fallback: text produces non-white pixels");
    return gFailed == prev;
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *renderPath = (argc > 1) ? argv[1] : "/dev/dri/renderD128";

    /* Bootstrap: open GPU device, create context, publish to blitter factory.
     * This is normally done by QEtnaviv2dIntegration::initialize(). */
    etna2d_device  *dev = nullptr;
    etna2d_context *ctx = nullptr;

    if (etna2d_device_open(renderPath, &dev) != ETNA2D_OK || !dev) {
        fprintf(stderr, "[FAIL] etna2d_device_open(%s)\n", renderPath);
        return 1;
    }
    if (etna2d_context_create(dev, 512, &ctx) != ETNA2D_OK || !ctx) {
        fprintf(stderr, "[FAIL] etna2d_context_create\n");
        etna2d_device_close(dev);
        return 1;
    }
    printf("[ OK ] GPU device/context ready\n");

    /* A QGuiApplication is needed for Qt font/platform subsystem. */
    QGuiApplication app(argc, argv);

    /* Publish context before any QPixmap is created. */
    EtnaBlittablePlatformPixmap::setGlobalContext(ctx, dev);

    testFillRect();
    testDrawPixmap();
    testStretchBlit();
    testRasterFallback();

    etna2d_context_destroy(ctx);
    etna2d_device_close(dev);

    if (gFailed == 0) {
        printf("\n[ OK ] All blitter tests PASSED\n");
        return 0;
    } else {
        printf("\n[FAIL] %d test(s) FAILED\n", gFailed);
        return 1;
    }
}
