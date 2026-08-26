# qt_ui_client.pro — Milestone B validation app
#
# Builds a minimal Qt app that exercises the etnaviv2d QPA in plane=wayland
# mode.  Links only Qt widgets — no custom HAL or KMS deps needed here.
#
# Built by wayland/Makefile as an out-of-tree qmake build.

TARGET = qt_ui_client

QT += core gui widgets

CONFIG += c++14

SOURCES = qt_ui_client.cpp
