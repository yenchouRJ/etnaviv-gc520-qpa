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

# A/B regression oracle: build the frozen legacy-KMS path (drmModeSetCrtc /
# drmModePageFlip, primary-only) instead of the default atomic path.
#   qmake etnaviv2d.pro LEGACY_KMS=1
!isEmpty(LEGACY_KMS):!equals(LEGACY_KMS, 0): DEFINES += ETNAVIV2D_LEGACY_KMS

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
    main.cpp                         \
    qetnaviv2dintegration.cpp        \
    qetnaviv2dscreen.cpp             \
    qetnaviv2dblitter.cpp            \
    qetnaviv2dbackingstore.cpp       \
    qetnaviv2dwaylandtransport.cpp

HEADERS = \
    qetnaviv2dintegration.h          \
    qetnaviv2dscreen.h               \
    qetnaviv2dblitter.h              \
    qetnaviv2dbackingstore.h         \
    qetnaviv2dwaylandtransport.h

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

# -----------------------------------------------------------------------
# Wayland protocol code generation (Milestone B)
#
# wayland-scanner is a host tool; the XMLs live in the sysroot's
# wayland-protocols datadir.  We generate two C sources + two headers:
#   xdg-shell              (xdg_wm_base / xdg_surface / xdg_toplevel)
#   linux-dmabuf-unstable-v1 (zwp_linux_dmabuf_v1)
#
# The generated files land in the qmake build directory ($$OUT_PWD) so
# they are never committed.  Header-only .h rules use variable_out=HEADERS
# so moc/compiler picks them up; .c rules use variable_out=SOURCES.
# -----------------------------------------------------------------------
WL_SCANNER   = $$system(which wayland-scanner 2>/dev/null)
isEmpty(WL_SCANNER) {
    WL_SCANNER = /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/bin/wayland-scanner
}
WL_PROTO_DIR = /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/aarch64-nuvoton-linux-gnu/sysroot/usr/share/wayland-protocols

# xdg-shell — private-code (C source)
xdg_shell_src.name   = xdg-shell-protocol.c
xdg_shell_src.input  = XDG_SHELL_XML
xdg_shell_src.output = xdg-shell-protocol.c
xdg_shell_src.commands = $$WL_SCANNER private-code ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
xdg_shell_src.variable_out = SOURCES
QMAKE_EXTRA_COMPILERS += xdg_shell_src
XDG_SHELL_XML = $$WL_PROTO_DIR/stable/xdg-shell/xdg-shell.xml

# xdg-shell — client-header (H header)
xdg_shell_hdr.name   = xdg-shell-client-protocol.h
xdg_shell_hdr.input  = XDG_SHELL_XML
xdg_shell_hdr.output = xdg-shell-client-protocol.h
xdg_shell_hdr.commands = $$WL_SCANNER client-header ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
xdg_shell_hdr.variable_out = HEADERS
QMAKE_EXTRA_COMPILERS += xdg_shell_hdr

# linux-dmabuf-unstable-v1 — private-code (C source)
dmabuf_src.name   = linux-dmabuf-unstable-v1-protocol.c
dmabuf_src.input  = DMABUF_XML
dmabuf_src.output = linux-dmabuf-unstable-v1-protocol.c
dmabuf_src.commands = $$WL_SCANNER private-code ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
dmabuf_src.variable_out = SOURCES
QMAKE_EXTRA_COMPILERS += dmabuf_src
DMABUF_XML = $$WL_PROTO_DIR/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml

# linux-dmabuf-unstable-v1 — client-header (H header)
dmabuf_hdr.name   = linux-dmabuf-unstable-v1-client-protocol.h
dmabuf_hdr.input  = DMABUF_XML
dmabuf_hdr.output = linux-dmabuf-unstable-v1-client-protocol.h
dmabuf_hdr.commands = $$WL_SCANNER client-header ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
dmabuf_hdr.variable_out = HEADERS
QMAKE_EXTRA_COMPILERS += dmabuf_hdr

# ivi-application (Milestone IVI) — XML lives in the Weston build tree
IVI_APP_XML = /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/weston-10.0.5/protocol/ivi-application.xml

ivi_app_src.name   = ivi-application-protocol.c
ivi_app_src.input  = IVI_APP_XML
ivi_app_src.output = ivi-application-protocol.c
ivi_app_src.commands = $$WL_SCANNER private-code ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
ivi_app_src.variable_out = SOURCES
QMAKE_EXTRA_COMPILERS += ivi_app_src

ivi_app_hdr.name   = ivi-application-client-protocol.h
ivi_app_hdr.input  = IVI_APP_XML
ivi_app_hdr.output = ivi-application-client-protocol.h
ivi_app_hdr.commands = $$WL_SCANNER client-header ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
ivi_app_hdr.variable_out = HEADERS
QMAKE_EXTRA_COMPILERS += ivi_app_hdr

# Generated headers are in OUT_PWD — add to include path
INCLUDEPATH += $$OUT_PWD

# libwayland-client (target sysroot)
SYSROOT = /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/aarch64-nuvoton-linux-gnu/sysroot
LIBS += -L$$SYSROOT/usr/lib -lwayland-client
INCLUDEPATH += $$SYSROOT/usr/include

OTHER_FILES += etnaviv2d.json

PLUGIN_TYPE       = platforms
PLUGIN_CLASS_NAME = QEtnaviv2dIntegrationPlugin
!equals(TARGET, $$QT_DEFAULT_QPA_PLUGIN): PLUGIN_EXTENDS = -
load(qt_plugin)
