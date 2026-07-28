# test_blitter — Step 1 unit test (no display needed)
#
# Build:
#   qmake test_blitter.pro && make -j$(nproc)
#
# Run on target:
#   QT_ETNAVIV2D_DEBUG=1 ./test_blitter [/dev/dri/renderD128]

TARGET = test_blitter

QT += gui gui-private

SOURCES = test_blitter.cpp \
          ../../src/qetnaviv2dblitter.cpp

HEADERS = ../../src/qetnaviv2dblitter.h

INCLUDEPATH += ../../src

# libetna2d
isEmpty(ETNA2D_INCDIR): ETNA2D_INCDIR = $$PWD/../../../hal/libetna2d
isEmpty(ETNA2D_LIBDIR): ETNA2D_LIBDIR = $$PWD/../../../hal/libetna2d/build
INCLUDEPATH += $$ETNA2D_INCDIR
LIBS        += -L$$ETNA2D_LIBDIR -letna2d

LIBS += -ldrm -ldrm_etnaviv

CONFIG += console
CONFIG -= app_bundle
