#!/bin/sh
# Generate H.264 test streams + NV12 reference + per-frame md5 (host, needs ffmpeg+libx264).
# usage: ./gen_streams.sh [outdir]     default: ../../videos
set -e
O=${1:-$(dirname "$(readlink -f "$0")")/../../videos}
mkdir -p "$O"; cd "$O"
SRC() { echo "testsrc2=size=$1:rate=30,format=yuv420p"; }
# name size frames x264-opts
gen() {
	n=$1; sz=$2; fr=$3; shift 3
	ffmpeg -y -v error -f lavfi -i "$(SRC $sz)" -frames:v $fr -c:v libx264 -pix_fmt yuv420p \
		-x264-params "$*:repeat-headers=1:aud=0" -f h264 $n.h264
	# reference: software decode to raw NV12 (no scaling, no frame drops)
	ffmpeg -y -v error -i $n.h264 -f rawvideo -pix_fmt nv12 -fps_mode passthrough $n.nv12
	w=${sz%x*}; h=${sz#*x}
	fs=$((w*h*3/2)); echo "$w $h $fs" > $n.info
	split -b $fs -d -a 4 $n.nv12 /tmp/_fr_$n.
	md5sum /tmp/_fr_$n.* | awk '{print $1}' > $n.md5; rm -f /tmp/_fr_$n.*
	echo "$n: $(wc -l < $n.md5) frames, $(stat -c %s $n.h264) bytes"
}
# S1 Baseline CAVLC I-only
gen s1_bp_i_320x240      320x240  10 profile=baseline:keyint=1:bframes=0:cabac=0
# S2 Baseline P frames, 1 ref
gen s2_bp_p_320x240      320x240  30 profile=baseline:keyint=30:bframes=0:ref=1:cabac=0
# S3 Main CABAC, 3 refs, no B
gen s3_mp_cabac_640x480  640x480  30 profile=main:keyint=30:bframes=0:ref=3:cabac=1
# S4 High, 8x8dct, B frames with reordering
gen s4_hp_b_640x480      640x480  30 profile=high:keyint=30:bframes=3:b-pyramid=none:ref=3:8x8dct=1
# S5 same at 720p
gen s5_hp_b_1280x720    1280x720  60 profile=high:keyint=30:bframes=3:b-pyramid=none:ref=3:8x8dct=1
