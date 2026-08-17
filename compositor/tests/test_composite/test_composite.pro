# test_composite — Step 4: single-committer dual-plane video+UI (plane=composite)
#
# First stage: streaming NV12 video on the primary plane only.
# The overlay (UI/HUD) is wired but transparent; the primary carries
# the decoded raw NV12 frames from earth_nv12_1024x600.raw.
#
# Build:
#   (from compositor/) make test_composite
#
# Run on target:
#   QT_QPA_PLATFORM=etnaviv2d QT_ETNAVIV2D_PLANE=composite \
#       ./test_composite --raw-video=./images/earth_nv12_1024x600.raw \
#                        --video-size=1024x600 [--frames=N] [--verbose]

TARGET = test_composite

QT += widgets gui-private fb_support-private
qtHaveModule(kms_support-private): QT += kms_support-private

SOURCES  = test_composite.cpp \
           $$PWD/../../../qpa/src/qetnaviv2dscreen.cpp

HEADERS  = $$PWD/../../../qpa/src/qetnaviv2dscreen.h

CONFIG += console
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../../../qpa/src \
               $$PWD/../../../hal/libetna2d \
               $$PWD/../../../hal/kms

LIBS += -L$$PWD/../../../hal/libetna2d/build -letna2d \
        -ldrm -ldrm_etnaviv
