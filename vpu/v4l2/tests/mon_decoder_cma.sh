# usage: mon3.sh <file> <fakesink|waylandsink>
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1
cd /mnt/vc8k
F=$1; SINK=$2
sync; echo 3 > /proc/sys/vm/drop_caches
cma(){ awk '/^CmaFree/{printf "CmaFree=%s ",$2}/^MemAvailable/{printf "MemAvail=%s ",$2}' /proc/meminfo; }
echo "baseline: $(cma)"
gst-launch-1.0 -q filesrc location=videos/$F ! h264parse ! v4l2slh264dec ! $SINK >/tmp/gst.log 2>&1 &
G=$!
MIN=999999; PK=0
t=0
while kill -0 $G 2>/dev/null; do
  sleep 1; t=$((t+1))
  c=$(awk '/^CmaFree/{print $2}' /proc/meminfo)
  [ "$c" -lt "$MIN" ] && MIN=$c
  if [ $((t%4)) -eq 1 ]; then
    n=$(grep -c "^size" /sys/kernel/debug/dma_buf/bufinfo 2>/dev/null)
    echo "t=$t gst_rss=$(awk '/^VmRSS/{print $2}' /proc/$G/status 2>/dev/null) gst_fd_dmabuf=$(ls -l /proc/$G/fd 2>/dev/null | grep -c dmabuf) $(cma)"
  fi
done
echo "--- min CmaFree during run: $MIN kB"
echo "--- end: $(cma)"
sed "s/\x1b\[[0-9;]*m//g" /tmp/gst.log | grep -iE "error|warn" | head -3
