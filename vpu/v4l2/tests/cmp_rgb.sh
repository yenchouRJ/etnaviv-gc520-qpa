#!/bin/sh
# Host: ./cmp_rgb.sh <outdir with *.BGRx/*.RGB16 from run_rgb.sh> <videodir>
# Reference = ffmpeg decode + bicubic scale + the same matrix (BT.601 limited) to bgr0 / rgb565le.
# Pass: frame count exact; unscaled (same size as the stream) PSNR >= 40 dB (matrix/rounding only);
# scaled PSNR >= 29 dB on every frame (the HW scales/upsamples chroma differently from ffmpeg
# bicubic: luma rms ~1, unbiased chroma error at colour edges, measured 30 dB on 720p->1024x600).
O=${1:?outdir}; V=${2:?videodir}; rc=0
for f in $O/*.BGRx $O/*.RGB16; do
	[ -f $f ] || continue
	b=$(basename $f); e=${b##*.}; b=${b%.*}; w=${b##*_}; h=${w#*x}; w=${w%x*}; s=${b%_*}
	case $e in BGRx) pf=bgr0; bpp=4;; RGB16) pf=rgb565le; bpp=2;; esac
	want=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $V/$s.h264)
	sz=$(stat -c %s $f); fs=$((w*h*bpp)); got=$((sz/fs))
	ffmpeg -v error -y -i $V/$s.h264 -vf "scale=$w:$h:flags=bicubic,scale=in_color_matrix=bt601:in_range=tv:out_range=pc,format=$pf" -f rawvideo /tmp/ref.rgb
	ffmpeg -v error -f rawvideo -pix_fmt $pf -s ${w}x$h -i $f -f rawvideo -pix_fmt $pf -s ${w}x$h -i /tmp/ref.rgb \
		-lavfi "[0][1]psnr=stats_file=/tmp/psnr.log" -f null - 2>/dev/null
	psnr=$(awk -F'psnr_avg:' 'NF>1{split($2,a," ");v=a[1]; if(min==""||v<min)min=v; s+=v; n++} END{printf "%.1f/%.1f", s/n, min}' /tmp/psnr.log)
	r=PASS; [ "$got" = "$want" ] && [ $((sz%fs)) -eq 0 ] || r=FAIL
	sw=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x $V/$s.h264)
	lim=29; [ "$sw" = "${w}x$h" ] && lim=40
	awk -v m=${psnr#*/} -v l=$lim 'BEGIN{exit !(m>=l)}' || r=FAIL
	echo "$r $b.$e frames $got/$want PSNR avg/min $psnr dB"
	[ $r = PASS ] || rc=1
done
exit $rc
