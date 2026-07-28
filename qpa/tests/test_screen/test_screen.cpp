/*
 * test_screen.cpp — Step 2: KMS screen test (no Qt widget, display required).
 *
 * Tests QEtnaviv2dScreen::initialize() in isolation:
 *  1. Opens card0 + renderD128.
 *  2. Allocates scanout BOs, does modeset.
 *  3. GPU-fills each scanout surface with a solid colour.
 *  4. Flips to display the colour for 1 second each.
 *  5. Cleans up (restores CRTC).
 *
 * Expected result on the panel:
 *   - Frame 0: solid red   (1 s)
 *   - Frame 1: solid green (1 s)
 *   - Frame 0 again via flip: solid blue (1 s)
 *   - Restored CRTC on exit.
 *
 * Run:
 *   QT_ETNAVIV2D_DEBUG=1 ./test_screen [/dev/dri/card0] [/dev/dri/renderD128]
 *
 * This test does NOT require a running Qt event loop.
 */
#include <QCoreApplication>
#include <QDebug>
#include <unistd.h>
#include <cstdio>

extern "C" {
#include "etna2d.h"
}

#include "../../src/qetnaviv2dblitter.h"
#include "../../src/qetnaviv2dscreen.h"

static int gFailed = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "[FAIL] %s\n", msg); ++gFailed; } \
    else          { printf("[ OK ] %s\n", msg); } \
} while (0)

int main(int argc, char **argv)
{
    const char *cardPath   = (argc > 1) ? argv[1] : "/dev/dri/card0";
    const char *renderPath = (argc > 2) ? argv[2] : "/dev/dri/renderD128";

    QCoreApplication app(argc, argv);

    /* --- Open GPU --- */
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

    EtnaBlittablePlatformPixmap::setGlobalContext(ctx, dev);

    /* --- Create and initialize screen --- */
    QEtnaviv2dScreen screen(QString::fromLocal8Bit(cardPath), ctx, dev);
    CHECK(screen.initialize(), "QEtnaviv2dScreen::initialize()");

    if (gFailed) goto cleanup;

    printf("[ OK ] Screen geometry: %dx%d\n",
           screen.geometry().width(), screen.geometry().height());
    printf("[ OK ] Physical size:   %.1fx%.1f mm\n",
           screen.physicalSize().width(), screen.physicalSize().height());
    CHECK(screen.geometry().width()  > 0, "screen width > 0");
    CHECK(screen.geometry().height() > 0, "screen height > 0");

    {
        int w = screen.geometry().width();
        int h = screen.geometry().height();

        /* Frame 0: fill back surface RED, flip */
        printf("\n  Filling red (1 s)...\n");
        etna2d_fill(ctx, screen.backSurface(), 0, 0, w, h, 0xffff0000u);
        CHECK(etna2d_finish(ctx) == ETNA2D_OK, "etna2d_finish red");
        CHECK(screen.pageFlip() == 0, "pageFlip red");
        sleep(1);

        /* Frame 1: fill NEW back surface GREEN, flip */
        printf("  Filling green (1 s)...\n");
        etna2d_fill(ctx, screen.backSurface(), 0, 0, w, h, 0xff00ff00u);
        CHECK(etna2d_finish(ctx) == ETNA2D_OK, "etna2d_finish green");
        CHECK(screen.pageFlip() == 0, "pageFlip green");
        sleep(1);

        /* Frame 2: fill BLUE, flip */
        printf("  Filling blue (1 s)...\n");
        etna2d_fill(ctx, screen.backSurface(), 0, 0, w, h, 0xff0000ffu);
        CHECK(etna2d_finish(ctx) == ETNA2D_OK, "etna2d_finish blue");
        CHECK(screen.pageFlip() == 0, "pageFlip blue");
        sleep(1);

        printf("  Screen test done; CRTC will be restored on exit.\n");
    }

cleanup:
    etna2d_context_destroy(ctx);
    etna2d_device_close(dev);

    if (gFailed == 0) {
        printf("\n[ OK ] All screen tests PASSED\n");
        return 0;
    }
    printf("\n[FAIL] %d test(s) FAILED\n", gFailed);
    return 1;
}
