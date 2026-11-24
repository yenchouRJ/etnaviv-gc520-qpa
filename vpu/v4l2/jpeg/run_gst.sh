#!/bin/sh
# Board: stock v4l2jpegdec over the whole set.  ./run_gst.sh [caps-extra e.g. ",width=1024,height=600"] [fmt NV12|xRGB|RGB16] [outdir]
# Writes out/<outdir>/<name>.<ext> and <name>.size ("W H W H" of the negotiated caps) for cmp_jpeg.sh
D=${D:-/mnt/vc8k/jpeg}; cd $D || exit 1
X=$1; F=${2:-NV12}; O=out/${3:-gst}; mkdir -p $O
case $F in NV12) E=nv12;; xRGB) E=xrgb;; *) E=rgb565;; esac
for f in img/j*.jpg; do
	b=$(basename $f .jpg); rm -f $O/$b.$E
	dmesg -c >/dev/null 2>&1
	r=$(gst-launch-1.0 -v filesrc location=$f ! jpegparse ! v4l2jpegdec ! "video/x-raw,format=$F$X" ! filesink location=$O/$b.$E 2>&1)
	wh=$(echo "$r" | sed -n 's/.*filesink0.GstPad:sink: caps = video\/x-raw.*width=(int)\([0-9]*\), height=(int)\([0-9]*\).*/\1 \2/p' | tail -1)
	echo "$wh $wh" > $O/$b.size
	st=OK; echo "$r" | grep -q "Execution ended" || st=FAIL
	echo "$st $b caps=$wh size=$(stat -c %s $O/$b.$E 2>/dev/null) $(dmesg | grep -ciE 'timed out|BUG|Oops|not supported') dmesg-err"
done
