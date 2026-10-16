export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
cd /mnt/vc8k
sync; echo 3 > /proc/sys/vm/drop_caches
W=$(pidof weston | cut -d' ' -f1)
gst-launch-1.0 -q filesrc location=videos/s9_bp_1024x600_real.h264 ! h264parse ! v4l2slh264dec ! waylandsink >/tmp/gst.log 2>&1 &
G=$!
for t in 1 2 3 4 5 6 7 8 9; do
  sleep 3
  echo "t=$((t*3)) gst_rss=$(awk '/^VmRSS/{print $2}' /proc/$G/status) gst_pss=$(awk '/^Pss:/{a+=$2}END{print a}' /proc/$G/smaps) weston_rss=$(awk '/^VmRSS/{print $2}' /proc/$W/status) $(awk '/^MemAvailable|^Cached|^CmaFree/{printf "%s%s ",$1,$2}' /proc/meminfo)"
done
wait $G
