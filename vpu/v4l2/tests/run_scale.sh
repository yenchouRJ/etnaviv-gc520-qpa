#!/bin/sh
# On the board: ./run_scale.sh [videodir]   decodes through the PP scaler, raw NV12 to $OUT.
# Then on the host: ./cmp_scale.sh <outdir> <videodir>
# Needs ma35d1_force_pp=0 and the patched v4l2codecs plugin (stepwise sizes, S_FMT from the
# downstream caps size, S_SELECTION crop; see patches/gst1-plugins-bad/).
D=${1:-/mnt/vc8k/videos}
OUT=${OUT:-/mnt/vc8k/out/scale}; mkdir -p $OUT
TMO=${TMO:-120}
# stream  PP_W x PP_H
CASES=${CASES:-"s5_hp_b_1280x720:1024x600 s5_hp_b_1280x720:640x360 s5_hp_b_1280x720:1024x720 s5_hp_b_1280x720:1280x600 s4_hp_b_640x480:1024x600 s4_hp_b_640x480:320x240 s4_hp_b_640x480:960x480 s3_mp_cabac_640x480:512x384 s1_bp_i_320x240:640x480 s6_bp_1080p:1024x600 s6_bp_1080p:640x360"}
for c in $CASES; do
	s=${c%%:*}; sz=${c#*:}; w=${sz%x*}; h=${sz#*x}
	O=$OUT/${s}_${w}x${h}.nv12; rm -f $O; dmesg -c >/dev/null 2>&1
	sync; echo 3 > /proc/sys/vm/drop_caches   # order-5 ctx allocation in hantro_open fails when fragmented
	echo "=== $s -> ${w}x${h}"
	gst-launch-1.0 -q filesrc location=$D/$s.h264 ! h264parse ! v4l2slh264dec ! \
		video/x-raw,format=NV12,width=$w,height=$h ! filesink location=$O &
	p=$!; t=0
	while kill -0 $p 2>/dev/null && [ $t -lt $TMO ]; do sleep 1; t=$((t+1)); done
	kill -0 $p 2>/dev/null && { echo "TIMEOUT, killing"; kill -9 $p; }
	wait $p 2>/dev/null; echo "gst exit $? (${t}s) size $(wc -c < $O 2>/dev/null)"
	dmesg | grep -iE "timed out|BUG|WARN|Oops" | tail -3
done
