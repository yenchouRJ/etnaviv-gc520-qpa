#!/bin/sh
# Host: ./cmp_jpeg.sh <outdir from run_jpeg.sh> [WxH]
#   unscaled (no WxH): compares the picture area of every <name>.nv12 with build/img/<name>.ref.nv12
#   scaled (WxH):      reference = ffmpeg decode + bicubic scale to WxH
# Pass: PSNR-Y >= 40 dB unscaled, >= 30 dB scaled.  Chroma PSNR is reported only (the HW and ffmpeg
# reduce 4:2:2/4:4:0/4:4:4/4:1:1 to 4:2:0 with different filters).
O=${1:?outdir}; S=$2
IMG=$(cd "$(dirname "$0")" && pwd)/build/img; rc=0; thr=40; [ -n "$S" ] && thr=30
for f in $O/*.nv12; do
	b=$(basename $f .nv12); [ -f $O/$b.size ] || continue
	set -- $(cat $O/$b.size); cw=$1; ch=$2
	set -- $(cat $IMG/$b.info); w=$1; h=$2
	if [ -z "$S" ]; then
		ref=$IMG/$b.ref.nv12; ow=$w; oh=$h
		crop="crop=$w:$h:0:0,"
	else
		ow=${S%x*}; oh=${S#*x}; ref=/tmp/jref.nv12; crop=""
		ffmpeg -v error -y -i $IMG/$b.jpg -vf "scale=$ow:$oh:flags=bicubic:in_range=pc:out_range=pc,format=nv12" -f rawvideo $ref
	fi
	# even picture size for the chroma pass (nv12 crop cannot do odd sizes); Y is compared as gray
	ew=$((ow & ~1)); eh=$((oh & ~1))
	psnr() { # filters for input 0 / input 1
		rm -f /tmp/jpsnr.log
		ffmpeg -v error -f rawvideo -pix_fmt nv12 -s ${cw}x$ch -i $f -f rawvideo -pix_fmt nv12 -s ${ow}x$oh -i $ref \
			-lavfi "[0:v]$1[a];[1:v]$2[b];[a][b]psnr=stats_file=/tmp/jpsnr.log" -f null - 2>/dev/null
		awk -v k=$3 '{for(i=1;i<=NF;i++){split($i,a,":"); if(a[1]==k)y=a[2]}} END{print y}' /tmp/jpsnr.log
	}
	if [ -n "$S" ]; then c0=""; else c0="crop=$ow:$oh:0:0,"; fi
	y=$(psnr "format=gray${c0:+,${c0%,}}" "format=gray" psnr_avg)
	ce="${c0}crop=$ew:$eh:0:0,format=yuv420p"
	u=$(psnr "$ce" "crop=$ew:$eh:0:0,format=yuv420p" psnr_u); v=$(psnr "$ce" "crop=$ew:$eh:0:0,format=yuv420p" psnr_v)
	r=PASS; awk -v p="$y" -v t=$thr 'BEGIN{exit !(p+0>=t)}' || r=FAIL
	echo "$r $b buf ${cw}x$ch pic ${ow}x$oh PSNR Y $y dB  U/V ${u:-n/a}/${v:-n/a} dB"
	[ $r = PASS ] || rc=1
done
exit $rc
