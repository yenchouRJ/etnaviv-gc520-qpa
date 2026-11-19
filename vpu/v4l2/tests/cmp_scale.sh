#!/bin/sh
# Host: ./cmp_scale.sh <outdir with *.nv12 from run_scale.sh> <videodir>
# Reference = ffmpeg decode + bicubic scale of the same stream.  Pass: frame count exact,
# PSNR-Y >= 30 dB and SSIM >= 0.95 (average and worst frame).
O=${1:?outdir}; V=${2:?videodir}; rc=0
for f in $O/*.nv12; do
	b=$(basename $f .nv12); w=${b##*_}; h=${w#*x}; w=${w%x*}; s=${b%_*}
	want=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $V/$s.h264)
	sz=$(stat -c %s $f); fs=$((w*h*3/2)); got=$((sz/fs))
	ffmpeg -v error -y -i $V/$s.h264 -vf scale=$w:$h:flags=bicubic -pix_fmt nv12 -f rawvideo /tmp/ref.nv12
	psnr=$(ffmpeg -v error -f rawvideo -pix_fmt nv12 -s ${w}x$h -i $f -f rawvideo -pix_fmt nv12 -s ${w}x$h -i /tmp/ref.nv12 \
		-lavfi "[0][1]psnr=stats_file=/tmp/psnr.log" -f null - 2>&1 >/dev/null; \
		awk -F'psnr_y:' 'NF>1{split($2,a," ");v=a[1]; if(min==""||v<min)min=v; s+=v; n++} END{printf "%.1f/%.1f", s/n, min}' /tmp/psnr.log)
	ssim=$(ffmpeg -v error -f rawvideo -pix_fmt nv12 -s ${w}x$h -i $f -f rawvideo -pix_fmt nv12 -s ${w}x$h -i /tmp/ref.nv12 \
		-lavfi "[0][1]ssim=stats_file=/tmp/ssim.log" -f null - 2>&1 >/dev/null; \
		awk -F'Y:' 'NF>1{split($2,a," ");v=a[1]; if(min==""||v<min)min=v; s+=v; n++} END{printf "%.3f/%.3f", s/n, min}' /tmp/ssim.log)
	r=PASS; [ "$got" = "$want" ] && [ $((sz%fs)) -eq 0 ] || r=FAIL
	awk -v p=${psnr#*/} -v q=${ssim#*/} 'BEGIN{exit !(p>=30 && q>=0.95)}' || r=FAIL
	echo "$r $b frames $got/$want PSNR-Y avg/min $psnr dB  SSIM avg/min $ssim"
	[ $r = PASS ] || rc=1
done
exit $rc
