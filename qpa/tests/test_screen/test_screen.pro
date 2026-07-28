# test_screen — Step 2: KMS screen test

TARGET = test_screen

QT += gui gui-private
qtHaveModule(kms_support-private): QT += kms_support-private
QT += fb_support-private

SOURCES = test_screen.cpp \
          ../../src/qetnaviv2dscreen.cpp \
          ../../src/qetnaviv2dblitter.cpp

HEADERS = ../../src/qetnaviv2dscreen.h \
          ../../src/qetnaviv2dblitter.h

INCLUDEPATH += ../../src

isEmpty(ETNA2D_INCDIR): ETNA2D_INCDIR = $$PWD/../../../hal/libetna2d
isEmpty(ETNA2D_LIBDIR): ETNA2D_LIBDIR = $$PWD/../../../hal/libetna2d/build
INCLUDEPATH += $$ETNA2D_INCDIR
LIBS        += -L$$ETNA2D_LIBDIR -letna2d

LIBS += -ldrm -ldrm_etnaviv

CONFIG += console
CONFIG -= app_bundle
