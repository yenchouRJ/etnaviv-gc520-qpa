#!/bin/sh
# TARGET (busybox sh): T5, CMA + fps of a 1080p H.264 decoded and scaled by the VPU PP to the panel size.
# usage: ./mon_cma_scaled.sh gst|cockpit [file.h264] [seconds]
#   gst     single gst pipeline: v4l2slh264dec ! capsfilter(panel size) ! fpsdisplaysink(waylandsink)
#   cockpit same video + the Qt cockpit UI (kiosk-shell), like demos/run_demo.sh but scaled
MODE=$1; F=${2:-/mnt/graphics_stack/demos/videos/cockpit_1080_30.h264}; SEC=${3:-30}
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
mkdir -p $XDG_RUNTIME_DIR
PM=$(cat /sys/class/drm/card*-*/modes | head -n 1); W=${PM%x*}; H=${PM#*x}
[ "$(cat /sys/module/*/parameters/ma35d1_force_pp 2>/dev/null)" = N ] || echo "WARN: check ma35d1_force_pp=N"
command -v systemctl >/dev/null && systemctl stop weston.service weston.socket 2>/dev/null
for p in $(pidof weston); do kill -9 $p 2>/dev/null; done; sleep 2
rm -f $XDG_RUNTIME_DIR/wayland-1 $XDG_RUNTIME_DIR/wayland-1.lock
TTY=--tty=1; [ "$(weston --version 2>/dev/null | sed 's/[^0-9]*\([0-9]*\).*/\1/')" -ge 11 ] 2>/dev/null && TTY=
weston $TTY --backend=drm-backend.so --config=/etc/xdg/weston/weston-kiosk.ini --shell=kiosk-shell.so --use-pixman >/tmp/weston.log 2>&1 &
n=0; while [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] && [ $n -lt 20 ]; do sleep 1; n=$((n+1)); done; sleep 2
sync; echo 3 > /proc/sys/vm/drop_caches
cma(){ awk '/^CmaFree/{printf "CmaFree=%s ",$2}/^MemAvailable/{printf "MemAvail=%s ",$2}' /proc/meminfo; }
BASE=$(awk '/^CmaFree/{print $2}' /proc/meminfo)
echo "panel=${W}x${H} file=$F mode=$MODE"; echo "baseline (weston up): $(cma)"
LOG=/tmp/t5_gst.log; : >$LOG
gst-launch-1.0 -v filesrc location=$F ! h264parse ! v4l2slh264dec ! \
  "video/x-raw,format=NV12,width=$W,height=$H" ! \
  fpsdisplaysink video-sink=waylandsink text-overlay=false sync=${SYNC:-true} \
  signal-fps-measurements=true fps-update-interval=1000 >$LOG 2>&1 &
G=$!
sleep 1; Q=
if [ "$MODE" = cockpit ]; then
  QT_QPA_PLATFORM=etnaviv2d:plane=wayland QT_ETNA2D_FMT=argb4444 QT2D_FPS=30 \
    cockpit --clip $(basename $F .h264) --cond kiosk-shell --fpslog $LOG >/tmp/t5_qt.log 2>&1 &
  Q=$!
fi
MIN=99999999; t=0
while [ $t -lt $SEC ] && kill -0 $G 2>/dev/null; do
  sleep 1; t=$((t+1))
  c=$(awk '/^CmaFree/{print $2}' /proc/meminfo); [ "$c" -lt "$MIN" ] && MIN=$c
  if [ $t -eq 8 ]; then
  # page cache of the clip lives in CMA too (movable): drop it to see the pinned (real) use
  echo "cached before drop: $(awk '/^Cached/{print $2}' /proc/meminfo) kB"
  sync; echo 1 > /proc/sys/vm/drop_caches; sleep 1
  PIN=$(awk '/^CmaFree/{print $2}' /proc/meminfo)
  fi
  [ $((t%5)) -eq 1 ] && echo "t=$t $(cma)"
done
kill -0 $G 2>/dev/null && killall -INT gst-launch-1.0 2>/dev/null; sleep 1
[ -n "$Q" ] && kill $Q 2>/dev/null
echo "--- pinned CmaFree (cache dropped, still playing): $PIN kB; baseline-pinned use = $((BASE-PIN)) kB"
echo "--- min CmaFree: $MIN kB (used by test: $(( $(awk '/^CmaTotal/{print $2}' /proc/meminfo) - MIN )) kB total in use)"
echo "--- fps samples (current:):"
sed "s/\x1b\[[0-9;]*m//g" $LOG | grep -o "current: [0-9.]*" | awk '{s+=$2;n++;if(min==""||$2<min)min=$2;if($2>mx)mx=$2}END{if(n)printf "n=%d avg=%.1f min=%.1f max=%.1f\n",n,s/n,min,mx}'
sed "s/\x1b\[[0-9;]*m//g" $LOG | grep -iE "error|warn" | head -3
dmesg | tail -5 | grep -iE "vc8k|hantro|cma|alloc" 
