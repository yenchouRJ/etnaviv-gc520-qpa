#!/bin/sh
# On the board: ./run_letterbox.sh [videodir]   decodes with add-borders=true into a larger/other-aspect
# CAPTURE size (video fitted + black bars written by the driver), raw NV12 to $OUT.
# Then on the host: ./cmp_letterbox.sh <outdir> <videodir>
D=${1:-/mnt/vc8k/videos}
OUT=${OUT:-/mnt/vc8k/out/letterbox}; mkdir -p $OUT
TMO=${TMO:-120}
CASES=${CASES:-"s4_hp_b_640x480:1024x600 s5_hp_b_1280x720:1024x768 s6_bp_1080p:1024x600 s1_bp_i_320x240:640x360 s11_hp_1024x600_drone:1920x1080"}
for c in $CASES; do
	s=${c%%:*}; sz=${c#*:}; w=${sz%x*}; h=${sz#*x}
	O=$OUT/${s}_${w}x${h}.nv12; rm -f $O; dmesg -c >/dev/null 2>&1
	sync; echo 3 > /proc/sys/vm/drop_caches
	echo "=== $s -> ${w}x${h} add-borders"
	gst-launch-1.0 -q filesrc location=$D/$s.h264 ! h264parse ! v4l2slh264dec add-borders=true ! \
		video/x-raw,format=NV12,width=$w,height=$h ! filesink location=$O &
	p=$!; t=0
	while kill -0 $p 2>/dev/null && [ $t -lt $TMO ]; do sleep 1; t=$((t+1)); done
	kill -0 $p 2>/dev/null && { echo "TIMEOUT, killing"; kill -9 $p; }
	wait $p 2>/dev/null; echo "gst exit $? (${t}s) size $(wc -c < $O 2>/dev/null)"
	dmesg | grep -iE "timed out|BUG|WARN|Oops|cma" | tail -3
done
