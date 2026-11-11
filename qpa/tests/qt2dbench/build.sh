#!/bin/sh
# Build qt2dbench with the in-tree qmake of the graphic Buildroot Qt (cross, aarch64).
QT_BUILD=${QT_BUILD:-/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_graphic/output/build/qt5base-da6e958319e95fe564d3b30c931492dd666bfaff}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/build" && cd "$HERE/build" && "$QT_BUILD/bin/qmake" "$HERE/qt2dbench.pro" && make -j4
