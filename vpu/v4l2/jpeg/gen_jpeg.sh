#!/bin/sh
# Host: ./gen_jpeg.sh [outdir]   default: ./build/img
# Builds the JPEG test set for the Hantro JPEG decoder (pp/plan.md T9) from the videos in
# ../../videos (Jelly.mov, drone-*.mp4), with libjpeg-turbo `cjpeg`/`jpegtran` and ffmpeg.
# For every positive image X.jpg it also writes X.ref.nv12 (reference decode, raw samples,
# full range, same size; 4:2:2/4:4:0/4:4:4/4:1:1 are reduced to 4:2:0 by ffmpeg, so compare
# Y strictly, chroma loosely) and X.info ("W H").
# Negative images (must be rejected cleanly, no hang) are named neg_*.jpg and have no .ref.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
V=$HERE/../../videos
O=${1:-$HERE/build/img}
mkdir -p "$O"; cd "$O"
T=$(mktemp -d); trap 'rm -rf $T' EXIT

# frame SRC TIME W H OUT.ppm : one frame, scaled to WxH
frame() { ffmpeg -y -v error -ss "$2" -i "$V/$1" -frames:v 1 -vf "scale=$3:$4:flags=bicubic" "$5"; }

# ref X.jpg : reference NV12, no range change (JFIF = full range both sides)
ref() {
	b=${1%.jpg}
	wh=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$1" | tr ',' ' ')
	echo "$wh" > "$b.info"
	ffmpeg -y -v error -i "$1" -vf "scale=in_range=pc:out_range=pc:flags=bicubic+accurate_rnd+full_chroma_int,format=nv12" \
		-f rawvideo "$b.ref.nv12"
}

# mk NAME SRC.ppm cjpeg-options... : encode + reference
mk() {
	n=$1; s=$2; shift 2
	cjpeg "$@" -outfile "$n.jpg" "$s"
	ref "$n.jpg"
	echo "$n: $(stat -c %s "$n.jpg") bytes"
}

frame Jelly.mov 3 1280 720 $T/jelly720.ppm
frame Jelly.mov 3 1920 1080 $T/jelly1080.ppm
frame drone-hd_1920_1080_30fps.mp4 5 1024 600 $T/drone600.ppm
frame drone-hd_1920_1080_30fps.mp4 5 1000 563 $T/drone563.ppm
frame drone-hd_1920_1080_30fps.mp4 5 1280 720 $T/drone720.ppm
frame drone-hd_1920_1080_30fps.mp4 5 48 48 $T/tiny48.ppm
frame drone-hd_1920_1080_30fps.mp4 5 100 60 $T/tiny100.ppm
frame drone-4k-60.mp4 10 4000 3000 $T/big12mp.ppm
frame drone-4k-60.mp4 10 3840 2160 $T/big4k.ppm

# --- baseline, sampling sweep (1280x720, multiple of 16) ---
mk j01_1280x720_420        $T/jelly720.ppm -quality 85 -sample 2x2,1x1,1x1
mk j02_1280x720_422        $T/jelly720.ppm -quality 85 -sample 2x1,1x1,1x1
mk j03_1280x720_440        $T/jelly720.ppm -quality 85 -sample 1x2,1x1,1x1
mk j04_1280x720_444        $T/jelly720.ppm -quality 85 -sample 1x1,1x1,1x1
mk j05_1280x720_411        $T/jelly720.ppm -quality 85 -sample 4x1,1x1,1x1
mk j06_1280x720_400        $T/jelly720.ppm -quality 85 -grayscale
# --- sizes ---
mk j10_1920x1080_420       $T/jelly1080.ppm -quality 85 -sample 2x2,1x1,1x1
mk j11_1024x600_420        $T/drone600.ppm  -quality 90 -sample 2x2,1x1,1x1
mk j12_1000x563_420        $T/drone563.ppm  -quality 75 -sample 2x2,1x1,1x1
mk j13_1000x563_422        $T/drone563.ppm  -quality 75 -sample 2x1,1x1,1x1
mk j14_1000x563_400        $T/drone563.ppm  -quality 75 -grayscale
mk j15_1000x563_444        $T/drone563.ppm  -quality 75 -sample 1x1,1x1,1x1
mk j16_48x48_420           $T/tiny48.ppm    -quality 85 -sample 2x2,1x1,1x1
mk j17_100x60_420          $T/tiny100.ppm   -quality 85 -sample 2x2,1x1,1x1
mk j18_3840x2160_420       $T/big4k.ppm     -quality 90 -sample 2x2,1x1,1x1
mk j19_4000x3000_420       $T/big12mp.ppm   -quality 90 -sample 2x2,1x1,1x1
# --- entropy coding / tables ---
mk j20_1280x720_420_q10    $T/drone720.ppm  -quality 10 -baseline -sample 2x2,1x1,1x1
mk j21_1280x720_420_q100   $T/drone720.ppm  -quality 100 -sample 2x2,1x1,1x1
mk j22_1280x720_420_opt    $T/drone720.ppm  -quality 85  -sample 2x2,1x1,1x1 -optimize
mk j23_1280x720_420_rst1   $T/drone720.ppm  -quality 85  -sample 2x2,1x1,1x1 -restart 1B
mk j24_1280x720_420_rst77  $T/drone720.ppm  -quality 85  -sample 2x2,1x1,1x1 -restart 77
mk j25_1280x720_422_rst1   $T/drone720.ppm  -quality 85  -sample 2x1,1x1,1x1 -restart 1B
mk j26_1280x720_420_dct    $T/drone720.ppm  -quality 85  -sample 2x2,1x1,1x1 -dct float

# --- negative: must be rejected cleanly (no hang, no oops) ---
cjpeg -quality 85 -progressive -sample 2x2,1x1,1x1 -outfile neg_progressive_1280x720.jpg $T/jelly720.ppm
cjpeg -quality 85 -arithmetic -outfile neg_arithmetic_1280x720.jpg $T/jelly720.ppm 2>/dev/null || rm -f neg_arithmetic_1280x720.jpg
# baseline, one scan per component (non-interleaved)
cat > $T/scans.txt <<EOF
0: 0 63 0 0;
1: 0 63 0 0;
2: 0 63 0 0;
EOF
jpegtran -copy none -scans $T/scans.txt -outfile neg_noninterleaved_1280x720.jpg j01_1280x720_420.jpg
# truncated (half) and corrupt (entropy data overwritten in the middle)
head -c $(( $(stat -c %s j01_1280x720_420.jpg) / 2 )) j01_1280x720_420.jpg > neg_truncated_1280x720.jpg
cp j01_1280x720_420.jpg neg_corrupt_1280x720.jpg
sz=$(stat -c %s j01_1280x720_420.jpg)
dd if=/dev/urandom of=neg_corrupt_1280x720.jpg bs=1 seek=$((sz/3)) count=4096 conv=notrunc 2>/dev/null
# garbage and zero-length
head -c 4096 /dev/urandom > neg_garbage.jpg
: > neg_empty.jpg
# too small (< 48 in both directions is allowed by the PP rules? 16x16 is below the HW minimum)
frame drone-hd_1920_1080_30fps.mp4 5 16 16 $T/t16.ppm
cjpeg -quality 85 -sample 2x2,1x1,1x1 -outfile neg_tiny_16x16.jpg $T/t16.ppm
# extended sequential (SOF1, 16-bit quant tables): not supported by the HW
cjpeg -quality 10 -sample 2x2,1x1,1x1 -outfile neg_sof1_16bitq.jpg $T/drone720.ppm
ls "$O" | grep -c '\.jpg$' | xargs echo "images:"
