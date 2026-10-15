#!/bin/sh
# On the board: ./run_v4.sh [videodir] [stream ...]   default dir /mnt/vc8k/videos, all streams
D=${1:-/mnt/vc8k/videos}; [ $# -gt 0 ] && shift
S=${*:-s1_bp_i_320x240 s2_bp_p_320x240 s3_mp_cabac_640x480 s4_hp_b_640x480 s5_hp_b_1280x720 s6_bp_1080p}
HERE=$(dirname "$0")
TMO=${TMO:-240}
OUT=${OUT:-/mnt/vc8k/out}; mkdir -p $OUT
rc=0
for s in $S; do
	echo "=== $s ==="; dmesg -c >/dev/null 2>&1
	O=$OUT/$s.out.nv12; rm -f $O
	# busybox has no timeout(1): run in background, kill after $TMO seconds
	# 1080 lines are decoded as 1088 + crop, so the NV12 buffer is padded: compare s6 in tight I420
	case $s in
	s6*) CONV="videoconvert ! video/x-raw,format=I420" ;;
	*)   CONV="video/x-raw,format=NV12" ;;
	esac
	gst-launch-1.0 -q filesrc location=$D/$s.h264 ! h264parse ! v4l2slh264dec ! \
		$CONV ! filesink location=$O &
	p=$!; t=0
	while kill -0 $p 2>/dev/null && [ $t -lt $TMO ]; do sleep 1; t=$((t+1)); done
	if kill -0 $p 2>/dev/null; then echo "TIMEOUT after ${TMO}s, killing gst"; kill -9 $p; fi
	wait $p 2>/dev/null; echo "gst exit $? (${t}s)"
	VDIR=$D sh $HERE/cmp_frames.sh $s $O || rc=1; rm -f $O
	dmesg | grep -iE "hantro|vpu|HW id|timed out|BUG|WARN|Oops" | tail -8
	[ $rc -ne 0 ] && { echo "stopping at first failure"; break; }
done
exit $rc
