export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
cd /mnt/vc8k
grep -E "^MemFree|^CmaFree" /proc/meminfo | tr '\n' ' '; echo " (baseline)"
dmesg -c >/dev/null
gst-launch-1.0 -q filesrc location=videos/s9_bp_1024x600_real.h264 ! h264parse ! v4l2slh264dec ! waylandsink >/tmp/gst.log 2>&1 &
G=$!
sleep 1
QT_QPA_PLATFORM=etnaviv2d:plane=wayland /mnt/vivante2d/wayland/qt_ui_client --no-gradient >/tmp/qt.log 2>&1 &
Q=$!
sleep 2
for t in 1 2 3 4 5 6 7 8; do
  printf "t=%s " $t
  for p in $G $Q; do printf "%s rss=%skB " $(cut -d' ' -f2 /proc/$p/comm 2>/dev/null; awk '/^Name/{n=$2}/^VmRSS/{print n}' /proc/$p/status) $(awk '/^VmRSS/{print $2}' /proc/$p/status); done
  grep -E "^MemFree|^CmaFree" /proc/meminfo | tr '\n' ' '
  awk '/^Pss:/{a+=$2}END{print "gst_pss="a"kB"}' /proc/$G/smaps
  sleep 3
done
echo "--- state"; grep -E "plane\[|fb=|format=|crtc-pos|src-pos" /sys/kernel/debug/dri/0/state | head -24
echo "--- gst fds"; ls -l /proc/$G/fd | grep -c dmabuf
kill -9 $Q 2>/dev/null; wait $G 2>/dev/null
dmesg | grep -v hantro | tail -4; tail -2 /tmp/gst.log
grep -E "^MemFree|^CmaFree" /proc/meminfo | tr '\n' ' '; echo " (end)"
