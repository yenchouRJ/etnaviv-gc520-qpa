#!/bin/sh
# usage: [HOLD=s] showj.sh image.jpg [caps]   stock pipeline on the panel via kiosk Weston; fdsrc keeps the last frame up
# Board convention (/etc/profile.d/weston.sh): XDG_RUNTIME_DIR=/tmp/xdg, Weston 10 socket wayland-1.
# Use the running Weston (boot one, desktop ini); only start a kiosk one if none is running.
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
mkdir -p $XDG_RUNTIME_DIR; chmod 0700 $XDG_RUNTIME_DIR
if [ ! -S $XDG_RUNTIME_DIR/wayland-1 ]; then
  weston --tty=1 --backend=drm-backend.so --config=/etc/xdg/weston/weston-kiosk.ini --shell=kiosk-shell.so --use-pixman >/tmp/weston.log 2>&1 &
  n=0; while [ ! -S $XDG_RUNTIME_DIR/wayland-1 ] && [ $n -lt 20 ]; do sleep 1; n=$((n+1)); done; sleep 2
fi
(cat $1; sleep ${HOLD:-6}) | gst-launch-1.0 -q fdsrc fd=0 blocksize=4000000 ! jpegparse ! v4l2jpegdec ! ${2:-video/x-raw,width=1024,height=600} ! waylandsink
