# test_glyph — Risk #3: text rendering performance measurement
#
# Benchmarks drawText (raster glyph SRC_OVER onto GPU surface) vs fillRect
# (GPU fast path) on etnaviv2d and linuxfb to quantify the cost of
# CPU raster fallback for text-heavy UIs.

TARGET = test_glyph

QT += widgets

SOURCES = test_glyph.cpp

CONFIG += console
CONFIG -= app_bundle
