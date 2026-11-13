#!/bin/sh
# Board (busybox sh). DDR-bandwidth / display-underflow A/B for the etnaviv2d QPA (modelled on vpu_bench.sh).
# Load: Weston ivi, a 1080p full-screen ARGB8888 Qt client on the primary plane (layer 1000, idle 1 fps, only
# there to make the primary scan out 1080p) + the qt2dbench --clip client on the overlay (layer 2000), 1080p ARGB8888.
#   ddr_bench.sh [tag] [secs]          env for the overlay client is inherited, e.g.
#   QT_ETNA2D_FMT=rgb565 sh ddr_bench.sh fmt565
#   sh ddr_bench.sh all   -> runs every A/B config below
# env: SPRITES(16) QT2D_FPS(overlay pace, 0=unpaced) BG=0 (no primary client) WARM(2) BIN OUT
# Output: one "DDRRESULT ..." line per run (also appended to $OUT/ddr_bench.log).
TAG=${1:-run}; SECS=${2:-10}
BIN=${BIN:-/mnt/graphics_stack/gpu/qt2dbench}; OUT=${OUT:-/mnt/graphics_stack/gpu/results}; mkdir -p "$OUT"
SPRITES=${SPRITES:-16}; WARM=${WARM:-2}; BG=${BG:-1}
DSEL=0x402614d0; DVAL=0x402614d8     # DC debug counters: 03 = primary 16B read bursts, 0C = overlay0, 05 = frames sent
PRI=0x40261518; OVL=0x40261540       # sticky underflow (read clears): primary bit5, overlay bit23
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1 QT_IVI_GEOMETRY=1920x1080
cpu_snap() { awk '/^cpu /{idle=$5+$6; tot=0; for(i=2;i<=NF;i++) tot+=$i; print tot, idle}' /proc/stat; }
dc() { devmem $DSEL 32 $1 >/dev/null; devmem $DVAL; }
upt() { awk '{printf "%d", $1*100}' /proc/uptime; }
alive_weston() { for p in $(pidof weston); do grep -q . /proc/$p/cmdline 2>/dev/null && return 0; done; return 1; }
start_weston() {
	alive_weston && return
	weston --tty=1 --backend=drm-backend.so --config=/etc/xdg/weston/weston-ivi.ini --shell=ivi-shell.so --use-pixman >/tmp/weston.log 2>&1 &
	n=0; while [ ! -S /tmp/xdg/wayland-1 ] && [ $n -lt 20 ]; do sleep 1; n=$((n+1)); done; sleep 2
}

if [ "$TAG" = all ]; then
	S=$0
	run() { t=$1; shift; env "$@" sh $S $t $SECS; }
	run base          X=1
	run fmt_rgb565    QT_ETNA2D_FMT=rgb565
	run fmt_xrgb8888  QT_ETNA2D_FMT=xrgb8888
	run cached        QT_ETNA2D_CACHED=1
	run text_cpu      QT_ETNA2D_GPU_TEXT=0
	run full_flush    QT_ETNA2D_FULL_FLUSH=1
	run no_batch      QT_ETNAVIV2D_NO_BATCH=1
	exit 0
fi

start_weston
SURF=2000
if [ "$BG" = 1 ]; then     # primary client: same plugin, idle, default format, so the primary plane scans out 1080p
	( env -u QT_ETNA2D_FMT QT_QPA_PLATFORM=etnaviv2d:plane=wayland QT_IVI_SURFACE_ID=1000 QT2D_FPS=1 \
	  $BIN --clip --stage-secs 600 --sprites 1 >/tmp/ddr_bg.log 2>&1 ) &
	BGP=$!; sleep 3
fi
( QT_QPA_PLATFORM=etnaviv2d:plane=wayland QT_IVI_SURFACE_ID=$SURF $BIN --clip --stage-secs $((SECS+WARM+4)) --sprites $SPRITES >/tmp/ddr_fg.log 2>&1 ) &
sleep 4
devmem $PRI >/dev/null; devmem $OVL >/dev/null
set -- $(cpu_snap); T0=$1; I0=$2
S0=$(date +%s); POLLS=0; SP=0; UP=0; UO=0; SUP=0; SUO=0; AT0=
while [ $(( $(date +%s) - S0 )) -lt $((SECS+WARM)) ]; do
	POLLS=$((POLLS+1)); W=0
	[ $(( $(date +%s) - S0 )) -ge $WARM ] && { W=1; SP=$((SP+1)); }
	if [ $W = 1 ]; then
		a=$(dc 3); b=$(dc 12); f=$(dc 5); t=$(upt)
		[ -z "$AT0" ] && { AV0=$a; AO0=$b; AF0=$f; AT0=$t; }
		AV1=$a; AO1=$b; AF1=$f; AT1=$t
	fi
	v=$(devmem $PRI 2>/dev/null); [ $(( v & 0x20 )) -ne 0 ] && { UP=$((UP+1)); [ $W = 1 ] && SUP=$((SUP+1)); }
	v=$(devmem $OVL 2>/dev/null); [ $(( v & 0x800000 )) -ne 0 ] && { UO=$((UO+1)); [ $W = 1 ] && SUO=$((SUO+1)); }
	usleep 250000
done
set -- $(cpu_snap); T1=$1; I1=$2
kill -9 $(pidof qt2dbench) 2>/dev/null; sleep 1
CPU=$(awk -v t=$((T1-T0)) -v i=$((I1-I0)) 'BEGIN{printf "%.0f", 100*(t-i)/t}')
DT=$((AT1-AT0)); [ $DT -le 0 ] && DT=1
bw() { awk -v n=$(( ($1-$2) & 0xFFFFFFFF )) -v d=$DT 'BEGIN{printf "%.0f", n*16/1e6/(d/100)}'; }
LINE="DDRRESULT tag=$TAG fmt=${QT_ETNA2D_FMT:-argb8888} cpu_sys_pct=$CPU uf_primary_steady=$SUP/$SP uf_overlay_steady=$SUO/$SP uf_primary_all=$UP/$POLLS axi_pri_rd_MBps=$(bw $AV1 $AV0) axi_ovl_rd_MBps=$(bw $AO1 $AO0) disp_fps=$(awk -v n=$(( (AF1-AF0) & 0xFFFFFFFF )) -v d=$DT 'BEGIN{printf "%.1f", n/(d/100)}') sprites=$SPRITES pace=${QT2D_FPS:-0} bg=$BG env=\"$(env | grep -E '^QT_ETNA' | tr '\n' ' ')\""
echo "$LINE"; echo "$LINE" >> "$OUT/ddr_bench.log"
[ -n "$BGP" ] && kill -9 $(pidof qt2dbench) 2>/dev/null
