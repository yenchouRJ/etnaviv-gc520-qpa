#!/bin/sh
# On the board (busybox sh), from /mnt/vc8k/jpeg:  ./run_jpeg.sh [unscaled|scaled|rgb|neg|all]
#   images in ./img (scp build/img/*.jpg), binary ./t_jpeg_dec, results in ./out/<mode>/
# Then on the host: ./cmp_jpeg.sh out/<mode> ...  (see cmp_jpeg.sh)
D=${D:-/mnt/vc8k/jpeg}; cd $D || exit 1
T=./t_jpeg_dec; I=$D/img; MODE=${1:-all}
POS=$(ls $I/j*.jpg 2>/dev/null)
SKIP=${SKIP:-}          # space separated name fragments to skip, e.g. SKIP="j19 j18"
run() { # outdir args...
	o=$1; shift; mkdir -p $o
	for f in $POS; do
		skip=0; for s in $SKIP; do case $f in *$s*) skip=1;; esac; done
		[ $skip = 1 ] && continue
		dmesg -c >/dev/null 2>&1
		$T -o $o "$@" $f
		dmesg | grep -iE "timed out|BUG|WARN|Oops|JPEG" | tail -2
	done
}
case $MODE in unscaled|all) echo "=== unscaled NV12"; run out/unscaled;; esac
case $MODE in scaled|all) echo "=== scaled 1024x600 NV12"; run out/scaled -s 1024x600;; esac
case $MODE in rgb|all) echo "=== XRGB 1024x600"; SKIP="$SKIP j19 j18"; run out/rgb -f xrgb -s 1024x600;; esac
case $MODE in neg|all)
	echo "=== negative (clean rejection expected)"
	for f in $I/neg_*.jpg; do
		dmesg -c >/dev/null 2>&1
		$T -x $f
		dmesg | grep -iE "timed out|BUG|WARN|Oops" | tail -2
	done;;
esac
