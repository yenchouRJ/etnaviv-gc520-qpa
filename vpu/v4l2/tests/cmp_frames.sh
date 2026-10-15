#!/bin/sh
# Compare decoded NV12 against reference md5 list (busybox-safe: dd/wc/md5sum only).
# usage: cmp_frames.sh <stream-base> <decoded.nv12>      VDIR = dir with <base>.info/.md5
b=$1; out=$2; V=${VDIR:-$(dirname "$0")/../../videos}
read w h fs < "$V/$b.info"
[ -f "$out" ] || { echo "FAIL: no output $out"; exit 2; }
sz=$(wc -c < "$out"); want=$(wc -l < "$V/$b.md5")
# Hantro H.264 capture buffers carry extra MV data after the picture; filesink writes the whole
# buffer. If size is a multiple of the frame count, use size/want as the per-buffer pitch.
pitch=$fs
if [ $((sz % want)) -eq 0 ] && [ $((sz / want)) -ge $fs ]; then pitch=$((sz / want)); fi
got=$((sz / pitch)); rem=$((sz % pitch))
echo "frames: got $got (size $sz, buffer pitch $pitch, visible $fs, rem $rem) want $want"
i=0; bad=0; first=
while [ $i -lt $got ]; do
	m=$(dd if="$out" bs=$pitch skip=$i count=1 2>/dev/null | head -c $fs | md5sum | cut -d' ' -f1)
	r=$(sed -n "$((i+1))p" "$V/$b.md5")
	if [ "$m" != "$r" ]; then bad=$((bad+1)); [ -z "$first" ] && first=$((i+1)); fi
	i=$((i+1))
done
if [ $bad -eq 0 ] && [ $got -eq $want ] && [ $rem -eq 0 ]; then echo "PASS $b"; exit 0; fi
echo "FAIL $b: $bad bad frame(s), first bad frame #${first:-none} (1-based)"; exit 1
