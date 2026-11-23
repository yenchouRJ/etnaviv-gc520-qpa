#!/bin/sh
# On the board: ./run_rgb.sh [videodir]   decodes through the PP to RGB (T7), raw frames to $OUT.
# Then on the host: ./cmp_rgb.sh <outdir> <videodir>
# Needs ma35d1_force_pp=0, the patched v4l2codecs plugin (RGB entries in gstv4l2format.c) and
# the kernel with RGB CAPTURE formats. FMT: BGRx (V4L2 XRGB32) or RGB16 (V4L2 RGB565).
D=${1:-/mnt/vc8k/videos}
OUT=${OUT:-/mnt/vc8k/out/rgb}; mkdir -p $OUT
TMO=${TMO:-120}
# stream  W x H  format (size = coded size gives a 1:1 pass through the PP)
CASES=${CASES:-"s1_bp_i_320x240:320x240:BGRx s1_bp_i_320x240:320x240:RGB16 s5_hp_b_1280x720:1024x600:BGRx s5_hp_b_1280x720:1024x600:RGB16 s4_hp_b_640x480:1024x600:BGRx s6_bp_1080p:1920x1080:BGRx s6_bp_1080p:1024x600:BGRx"}
for c in $CASES; do
	s=${c%%:*}; r=${c#*:}; sz=${r%%:*}; fmt=${r#*:}; w=${sz%x*}; h=${sz#*x}
	O=$OUT/${s}_${w}x${h}.$fmt; rm -f $O; dmesg -c >/dev/null 2>&1
	sync; echo 3 > /proc/sys/vm/drop_caches
	echo "=== $s -> ${w}x${h} $fmt"
	gst-launch-1.0 -q filesrc location=$D/$s.h264 ! h264parse ! v4l2slh264dec ! \
		video/x-raw,format=$fmt,width=$w,height=$h ! filesink location=$O &
	p=$!; t=0
	while kill -0 $p 2>/dev/null && [ $t -lt $TMO ]; do sleep 1; t=$((t+1)); done
	kill -0 $p 2>/dev/null && { echo "TIMEOUT, killing"; kill -9 $p; }
	wait $p 2>/dev/null; echo "gst exit $? (${t}s) size $(wc -c < $O 2>/dev/null)"
	dmesg | grep -iE "timed out|BUG|WARN|Oops" | tail -3
done
