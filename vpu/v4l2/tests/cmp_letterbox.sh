#!/bin/sh
# Host: ./cmp_letterbox.sh <outdir with *.nv12 from run_letterbox.sh> <videodir>
# Reference = ffmpeg decode, bicubic scale to the fitted rectangle, black pad to the full size.
# Pass: frame count exact, bars exactly black (Y=16, UV=128) in every frame, PSNR-Y >= 30 dB worst frame.
O=${1:?outdir}; V=${2:?videodir}; rc=0
for f in $O/*.nv12; do
	b=$(basename $f .nv12); W=${b##*_}; H=${W#*x}; W=${W%x*}; s=${b%_*}
	set -- $(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 $V/$s.h264 | tr ',' ' '); cw=$1; ch=$2
	set -- $(python3 -c "
cw,ch,W,H=$cw,$ch,$W,$H
if cw*H>ch*W: w=W; h=ch*W//cw
else: h=H; w=cw*H//ch
w&=~7; h&=~1
print(w,h,((W-w)//2)&~1,((H-h)//2)&~1)"); rw=$1; rh=$2; x=$3; y=$4
	want=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $V/$s.h264)
	sz=$(stat -c %s $f); fs=$((W*H*3/2)); got=$((sz/fs))
	ffmpeg -v error -y -i $V/$s.h264 -vf "scale=$rw:$rh:flags=bicubic,pad=$W:$H:$x:$y:black" -pix_fmt nv12 -f rawvideo /tmp/refl.nv12
	psnr=$(ffmpeg -v error -f rawvideo -pix_fmt nv12 -s ${W}x$H -i $f -f rawvideo -pix_fmt nv12 -s ${W}x$H -i /tmp/refl.nv12 \
		-lavfi "[0][1]psnr=stats_file=/tmp/psnrl.log" -f null - 2>&1 >/dev/null; \
		awk -F'psnr_y:' 'NF>1{split($2,a," ");v=a[1]; if(min==""||v<min)min=v; s+=v; n++} END{printf "%.1f/%.1f", s/n, min}' /tmp/psnrl.log)
	bars=$(python3 - $f $W $H $x $y $rw $rh <<'PY'
import sys, numpy as np
f,W,H,x,y,rw,rh=sys.argv[1],*map(int,sys.argv[2:])
a=np.fromfile(f,np.uint8); n=a.size//(W*H*3//2); a=a[:n*W*H*3//2].reshape(n,-1)
bad=0
for fr in a:
    Y=fr[:W*H].reshape(H,W); UV=fr[W*H:].reshape(H//2,W//2,2)
    my=np.ones((H,W),bool); my[y:y+rh,x:x+rw]=False
    mc=np.ones((H//2,W//2),bool); mc[y//2:(y+rh)//2,x//2:(x+rw)//2]=False
    bad+=(Y[my]!=16).sum()+(UV[mc]!=128).sum()
print("ok" if bad==0 else "BAD(%d)"%bad)
PY
)
	r=PASS; [ "$got" = "$want" ] && [ $((sz%fs)) -eq 0 ] || r=FAIL
	[ "$bars" = ok ] || r=FAIL
	awk -v p=${psnr#*/} 'BEGIN{exit !(p>=30)}' || r=FAIL
	echo "$r $b rect ${rw}x$rh@$x,$y frames $got/$want bars $bars PSNR-Y avg/min $psnr dB"
	[ $r = PASS ] || rc=1
done
exit $rc
