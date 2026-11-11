#!/bin/sh
# Step 5: GPU (etnaviv2d) vs CPU raster (offscreen platform) per op and size, offscreen, unpaced.
#   sh run_ops_ab.sh <tag> [outdir]     run on the target under weston ivi
TAG=${1:-ops}; OUT=${2:-./results}; mkdir -p "$OUT"
export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1 QT_IVI_SURFACE_ID=2000 QT_IVI_GEOMETRY=${GEOM:-1024x600}
QT_QPA_PLATFORM=etnaviv2d:plane=wayland ./qt2dbench --ops --secs ${SECS:-1} > "$OUT/$TAG-gpu.txt" 2>&1
QT_QPA_PLATFORM=offscreen QT_QPA_OFFSCREEN_NO_GLX=1 ./qt2dbench --ops --secs ${SECS:-1} > "$OUT/$TAG-cpu.txt" 2>&1
awk '
  FNR==1 { f++ }
  /^RESULT/ { op="";sz="";us="";for(i=1;i<=NF;i++){ if($i~/^op=/)op=substr($i,4); if($i~/^size=/)sz=substr($i,6); if($i~/^us_op=/)us=substr($i,7) }
              k=op" "sz; if(f==1){g[k]=us; order[++n]=k} else c[k]=us }
  END { printf "%-10s %-6s %10s %10s %8s\n","op","size","gpu_us","cpu_us","gpu/cpu";
        for(i=1;i<=n;i++){ k=order[i]; split(k,a," "); printf "%-10s %-6s %10.1f %10.1f %8.2f\n",a[1],a[2],g[k],c[k],g[k]/c[k] } }' "$OUT/$TAG-gpu.txt" "$OUT/$TAG-cpu.txt"
