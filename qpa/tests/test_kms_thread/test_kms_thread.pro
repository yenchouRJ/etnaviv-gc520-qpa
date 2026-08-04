# test_kms_thread — Risk #5: KMS threading stress test
#
# Two QThreads drive concurrent flush() calls into the same
# QEtnaviv2dScreen (same card0 fd, same m_flipPending) with no mutex.
#
# Thread A: video playback from pre-extracted PNGs (left half).
# Thread B: unthrottled bouncing circles with alpha-blend (right half).
#
# Tearing on the left half = Race #5 visible on hardware.
# After fix: no tearing, no EBUSY, smooth FPS on both halves.
#
# Usage:
#   QT_QPA_PLATFORM=etnaviv2d ./test_kms_thread [--frames=300] [--fps=60] [--circles=16]

TARGET = test_kms_thread

QT += widgets

SOURCES = test_kms_thread.cpp

CONFIG += console
CONFIG -= app_bundle
