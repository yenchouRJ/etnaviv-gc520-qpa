# etnaviv2d QPA plugin — qmake build file
#
# Build inside a configured Qt5 build tree (cross-compiled for aarch64):
#
#   source /path/to/environment-setup
#   qmake etnaviv2d.pro
#   make -j$(nproc)
#
# The plugin lands in $QTDIR/plugins/platforms/libqetnaviv2d.so
# Deploy to the target alongside the app binary.
#
# Tuning:
#   ETNA2D_LIBDIR  — path to libetna2d.a / libetna2d.so (default: ../hal/libetna2d/build)
#   ETNA2D_INCDIR  — path to etna2d.h                   (default: ../hal/libetna2d)
#   KMS_INCDIR     — path to kms_display.h               (default: ../hal/kms)

TARGET = qetnaviv2d

DEFINES += QT_NO_FOREACH

QT += \
    core-private          \
    gui-private           \
    service_support-private       \
    eventdispatcher_support-private \
    fontdatabase_support-private  \
    fb_support-private

# kms_support-private pulls in the libdrm sysroot include path (drm.h, xf86drm.h, ...)
qtHaveModule(kms_support-private): QT += kms_support-private

# Input support — same conditional guard as linuxfb
qtHaveModule(input_support-private): QT += input_support-private

SOURCES = \
    main.cpp                    \
    qetnaviv2dintegration.cpp   \
    qetnaviv2dscreen.cpp        \
    qetnaviv2dblitter.cpp       \
    qetnaviv2dbackingstore.cpp

HEADERS = \
    qetnaviv2dintegration.h     \
    qetnaviv2dscreen.h          \
    qetnaviv2dblitter.h         \
    qetnaviv2dbackingstore.h

INCLUDEPATH += $$PWD

# libetna2d (our userspace HAL)
isEmpty(ETNA2D_INCDIR): ETNA2D_INCDIR = $$PWD/../hal/libetna2d
isEmpty(ETNA2D_LIBDIR): ETNA2D_LIBDIR = $$PWD/../hal/libetna2d/build
INCLUDEPATH += $$ETNA2D_INCDIR
LIBS        += -L$$ETNA2D_LIBDIR -letna2d

# kms_display helper headers (for tests; not linked into the plugin itself)
isEmpty(KMS_INCDIR): KMS_INCDIR = $$PWD/../hal/kms
INCLUDEPATH += $$KMS_INCDIR

# libdrm (BO alloc, PRIME, etnaviv cmd_stream, KMS ioctls) — present in sysroot
LIBS += -ldrm -ldrm_etnaviv

OTHER_FILES += etnaviv2d.json

PLUGIN_TYPE       = platforms
PLUGIN_CLASS_NAME = QEtnaviv2dIntegrationPlugin
!equals(TARGET, $$QT_DEFAULT_QPA_PLUGIN): PLUGIN_EXTENDS = -
load(qt_plugin)
