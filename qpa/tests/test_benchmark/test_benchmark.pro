# test_benchmark — Step 4: GPU vs CPU frame-rate benchmark
#
# Build once, run with two different QT_QPA_PLATFORM values to compare.

TARGET = test_benchmark

QT += widgets

SOURCES = test_benchmark.cpp

CONFIG += console
CONFIG -= app_bundle
