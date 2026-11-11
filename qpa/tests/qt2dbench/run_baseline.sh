#!/bin/sh
# Step 0 baseline (improvement-plan.md). Run ON THE TARGET under weston ivi-shell.
#   sh run_baseline.sh <tag> [outdir]     e.g. sh run_baseline.sh step0 /mnt/graphics_stack/gpu/results
# Needs ./qt2dbench and the plugin in /usr/lib/qt/plugins/platforms.
TAG=${1:-step0}; OUT=${2:-./results}; mkdir -p "$OUT"
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
export QT_IVI_SURFACE_ID=2000 QT_ETNAVIV2D_DEBUG=2
B=./qt2dbench
run() { # name plat geometry fmt args...
  n=$1; p=$2; g=$3; f=$4; shift 4
  QT_QPA_PLATFORM=$p QT_IVI_GEOMETRY=$g QT_ETNA2D_FMT=$f "$@" > "$OUT/$TAG-$n.txt" 2>&1
  echo "== $n"; grep -E "^(STAGE|RESULT|VERIFY|MATRIX|FAIL)" "$OUT/$TAG-$n.txt" | tail -${TAILN:-12}
}
W=etnaviv2d:plane=wayland
for r in 1 2 3; do
  for g in 1024x600 1920x1080; do
    for f in argb8888 rgb565; do
      run clip-$g-$f-r$r $W $g $f $B --clip --stage-secs 6 --sprites 16
    done
  done
done
TAILN=60 run ops-1024x600 $W 1024x600 argb8888 $B --ops --secs 2
TAILN=60 run matrix-1024x600 $W 1024x600 argb8888 $B --matrix
