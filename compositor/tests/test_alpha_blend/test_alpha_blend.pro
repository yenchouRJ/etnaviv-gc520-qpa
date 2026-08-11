# test_alpha_blend — Step 3: overlay ARGB8888 + SRC_OVER blend verification
#
# Single-process test: QPA plugin (plane=primary) drives the primary plane;
# this test directly constructs a second QEtnaviv2dScreen for the overlay
# plane, sharing the primary's DRM master fd.
#
# We compile qetnaviv2dscreen.cpp directly into the test binary so we can
# instantiate QEtnaviv2dScreen without linking the whole plugin .so.
#
# Build:
#   (from compositor/) make test_alpha_blend
#
# Run on target:
#   QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=primary \
#       ./test_alpha_blend --image=./images/earth.png [--frames=300] [--verbose]

TARGET = test_alpha_blend

QT += widgets gui-private fb_support-private
qtHaveModule(kms_support-private): QT += kms_support-private

SOURCES  = test_alpha_blend.cpp \
           $$PWD/../../../qpa/src/qetnaviv2dscreen.cpp

HEADERS  = $$PWD/../../../qpa/src/qetnaviv2dscreen.h

CONFIG += console
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../../../qpa/src \
               $$PWD/../../../hal/libetna2d \
               $$PWD/../../../hal/kms

LIBS += -L$$PWD/../../../hal/libetna2d/build -letna2d \
        -ldrm -ldrm_etnaviv
