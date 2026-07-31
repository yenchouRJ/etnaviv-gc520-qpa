# test_slideshow — Risk #2: raster-source pixmap path on hardware
#
# Loads PNGs from disk (QImage::load → QPixmap::fromImage) and draws them
# full-screen via QPainter::drawPixmap / drawImage, exercising both:
#   - QPixmap (EtnaBlittable blitter→blitter) path
#   - QImage   (raw raster → WC-mapped backing BO CPU fallback) path
#
# Build:
#   (from qpa/) make test_slideshow
#
# Run on target:
#   QT_QPA_PLATFORM=etnaviv2d ./test_slideshow ../../images 800 3
#   QT_QPA_PLATFORM=linuxfb   ./test_slideshow ../../images 800 3

TARGET = test_slideshow

QT += widgets

SOURCES = test_slideshow.cpp

CONFIG += console
CONFIG -= app_bundle
