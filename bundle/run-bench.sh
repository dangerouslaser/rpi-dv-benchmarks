#!/bin/bash
# On the Raspberry Pi 5: probe the system, check correctness, then benchmark
# yblod's Vulkan engine against CB1 (GLES) on every clip given, from the Pi's
# hardware HEVC decoder (V4L2 request, SAND128).
#
#   ./run-bench.sh [options] CLIP...
#     --frames N        pictures per run (default 12: the decoder's 32-buffer pool
#                       limits hardware-decode runs to 12 held pictures)
#     --throughput S    seconds of flat-out rendering per engine (default 20)
#     --repeat N        performance runs per engine, engine order alternating (default 3)
#     --play S          seconds of real-time playback per engine (default 30; 0 skips)
#     --quick           correctness only (no performance runs)
#     --no-sw           skip the software-decode reference runs
#     --out DIR         results directory (default results/HOST-DATE)
#
# Per clip, in order (so the most informative results come first):
#   1. correctness: CB1 vs Vulkan output on hardware-decoded pictures, and the
#      Vulkan engine's output from hardware vs software decoding of the same
#      pictures (HEVC decoding is exact: identical output proves the SAND
#      layout handling end to end)
#   2. performance: each engine alone, REPEAT times, alternating which goes
#      first, clocks / temperature / throttling recorded around every run
#   3. options, per picture: the Vulkan engine's switches (specialized
#      pipelines, narrow variants, the Annex B tile) and FEL EL upscale
#      (Master Reference's Annex B or bilinear), CB1's Reference mode with
#      bilinear FEL
#   3b. real-time playback: decoding on its own thread, rendering on the
#      stream's frame clock; pictures that missed their display deadline;
#      each engine as Master Reference and with the bilinear compromise
#   4. the Vulkan engine's per-stage GPU times (DVVK_PROFILE), and the V3D
#      compiler's statistics for every shader both engines use (threads,
#      register spills: shader-stats.txt per clip)
#   5. the same from software decoding, for reference, with linear P010 and
#      with SAND input (the difference is the SAND conversion's cost)
# Writes DIR/system.txt, a log and JSON per run, DIR/summary.txt and
# DIR/table.txt (medians over repeats). Send the whole directory back.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BENCH="$HERE/bin/dvbench"
FRAMES=12 THROUGHPUT=20 REPEAT=3 PLAY=30 SW=1 QUICK=0 OUT=
CLIPS=()
while [ $# -gt 0 ]; do
  case $1 in
    --frames) FRAMES=$2; shift ;;
    --throughput) THROUGHPUT=$2; shift ;;
    --repeat) REPEAT=$2; shift ;;
    --play) PLAY=$2; shift ;;
    --quick) QUICK=1 ;;
    --no-sw) SW=0 ;;
    --out) OUT=$2; shift ;;
    -*) echo "unknown option $1"; exit 2 ;;
    *) CLIPS+=("$1") ;;
  esac
  shift
done
[ ${#CLIPS[@]} -gt 0 ] || { sed -n 2,27p "$0"; exit 2; }
OUT=${OUT:-results/$(hostname)-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

# The V3D render node (Vulkan and GLES both run there; vc4 is display only).
NODE=
for n in /sys/class/drm/renderD*; do
  [ "$(basename "$(readlink -f "$n/device/driver")")" = v3d ] && NODE=/dev/dri/$(basename "$n")
done

# Fixed CPU frequency policy for the whole session (restored at exit).
GOV_FILES=$(ls /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null)
GOV_BEFORE=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
GOV_NOTE="governor $GOV_BEFORE"
if [ -n "$GOV_FILES" ] && [ "$GOV_BEFORE" != performance ]; then
  if echo performance | sudo -n tee $GOV_FILES >/dev/null 2>&1; then
    GOV_NOTE="governor performance (was $GOV_BEFORE, restored at exit)"
    trap 'echo "$GOV_BEFORE" | sudo -n tee $GOV_FILES >/dev/null 2>&1' EXIT
  else
    GOV_NOTE="governor $GOV_BEFORE: WARNING, could not set performance (run with sudo rights, or set it yourself)"
  fi
fi

# The decoder's pictures live in CMA; Raspberry Pi OS's default (64 MB on some
# boards) is too small for 4K HEVC plus a Profile 7 EL.
CMA_MB=$(awk '/CmaTotal/ {print int($2 / 1024)}' /proc/meminfo)
CMA_NOTE="CMA ${CMA_MB:-?} MB"
if [ "${CMA_MB:-0}" -lt 256 ]; then
  CMA_NOTE="$CMA_NOTE: WARNING, too small for 4K decoding; add  dtoverlay=vc4-kms-v3d,cma-512  to /boot/firmware/config.txt and reboot"
  echo "$CMA_NOTE"
fi

snapshot() { # one line: time, temperature, throttle flags, clocks
  local t=$(vcgencmd measure_temp 2>/dev/null | cut -d= -f2)
  local th=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)
  local c=
  for k in arm core v3d hevc; do c="$c $k=$(vcgencmd measure_clock $k 2>/dev/null | cut -d= -f2)"; done
  echo "temp=$t throttled=$th$c"
}

{
  echo "== dvbench bundle"; cat "$HERE/VERSION" 2>/dev/null
  echo "== board"; tr -d '\0' </proc/device-tree/model 2>/dev/null; echo; uname -a
  grep -E "^(PRETTY_NAME|VERSION)=" /etc/os-release
  echo "== memory"; free -m
  echo "== v3d render node: ${NODE:-NOT FOUND}"; ls -l /dev/dri
  echo "== $GOV_NOTE"
  echo "== $CMA_NOTE"
  echo "== start: $(snapshot)"
  echo "== cooling"; ls /sys/devices/platform/cooling_fan* -d 2>/dev/null || echo "no fan device found: use an active cooler, the Pi 5 throttles at 80-85C"
  echo "== dmabuf allocators"; ls -l /dev/udmabuf /dev/dma_heap 2>&1
  echo "== V4L2 decoders"; v4l2-ctl --list-devices 2>&1 || echo "(v4l2-ctl not installed: apt install v4l-utils)"
  echo "== Mesa packages"; dpkg -l 'mesa-vulkan-drivers' 'libegl-mesa0' 2>/dev/null | grep ^ii
  echo "== Vulkan"; vulkaninfo --summary 2>&1 || echo "(vulkaninfo not installed: apt install vulkan-tools)"
  echo "== Vulkan features the engine cares about"
  vulkaninfo 2>/dev/null | grep -E "shaderInt64|shaderInt16|storageBuffer16BitAccess|storageBuffer8BitAccess|timelineSemaphore|synchronization2|maintenance4|VK_KHR_push_descriptor|VK_KHR_external_semaphore_fd|VK_EXT_external_memory_dma_buf|VK_EXT_image_drm_format_modifier|VK_EXT_queue_family_foreign|VK_EXT_physical_device_drm|timestampComputeAndGraphics|subgroupSize |maxComputeWorkGroupInvocations|maxComputeSharedMemorySize" | sort -u
  echo "== EGL/GLES"; eglinfo -B 2>&1 | head -40 || echo "(eglinfo not installed: apt install mesa-utils-bin)"
  echo "== dmabuf import probe"; "$HERE/bin/probe-dmabuf" 2>&1
} >"$OUT/system.txt" 2>&1
echo "system: $OUT/system.txt (v3d node ${NODE:-NOT FOUND}; $GOV_NOTE)"

bench() { # log, args...: one dvbench run, snapshots around it in the log
  local log=$1; shift
  { echo "== before: $(snapshot)"
    "$BENCH" ${NODE:+--node $NODE} "$@"
    local rc=$?
    echo "== after: $(snapshot)"
    echo "== exit $rc"
    return $rc; } >"$log" 2>&1
}
hashes() { grep "^hash $1 " "$2" | cut -d' ' -f3,4; }   # engine, log -> "N HASH" lines
compare() { # label, a, b: picture-by-picture identity of two "N HASH" lists
  local same total
  total=$(echo "$2" | grep -c .); same=$(comm -12 <(echo "$2" | sort) <(echo "$3" | sort) | grep -c .)
  if [ "$total" -gt 0 ] && [ "$same" = "$total" ]; then echo "   ok    $1: $same of $total pictures identical"
  else echo "   FAIL  $1: $same of $total pictures identical"; fi
}

i=0
for clip in "${CLIPS[@]}"; do
  i=$((i + 1))
  tag=$(printf "%02d" $i)-$(basename "$clip" | tr -c 'A-Za-z0-9\n' _ | cut -c1-32)
  echo "== $clip"

  # 1. Correctness.
  # (DVBENCH_HW_SOURCE=sw: test this script without a Pi; "hardware" is software then)
  hw=(--source "${DVBENCH_HW_SOURCE:-v4l2}")
  if ! bench "$OUT/$tag-check-hw.log" "${hw[@]}" --frames 12 --warmup 0 --hash "$clip"; then
    # Both layers on the one HEVC block failed: the EL in software instead.
    hw=(--source "${DVBENCH_HW_SOURCE:-v4l2}" --el-sw)
    bench "$OUT/$tag-check-hw.log" "${hw[@]}" --frames 12 --warmup 0 --hash "$clip" ||
      { echo "   FAIL  hardware decoding, see $OUT/$tag-check-hw.log"; continue; }
    echo "   note  Profile 7 EL decoded in software (both layers failed on the HEVC block)"
  fi
  grep -E "^(source|decode):" "$OUT/$tag-check-hw.log" | sed 's/^/   /'
  compare "CB1 == Vulkan (hardware decode)" "$(hashes cb1 "$OUT/$tag-check-hw.log")" \
    "$(hashes yblod-vulkan "$OUT/$tag-check-hw.log")"
  bench "$OUT/$tag-check-sw.log" --source sw --layout sand --engines vk --frames 12 --warmup 0 --hash "$clip"
  compare "Vulkan: hardware decode == software decode (SAND layout end to end)" \
    "$(hashes yblod-vulkan "$OUT/$tag-check-hw.log")" "$(hashes yblod-vulkan "$OUT/$tag-check-sw.log")"
  [ $QUICK = 1 ] && continue

  # 2. Performance: each engine alone, alternating order, REPEAT times.
  for rep in $(seq 1 "$REPEAT"); do
    order="cb1 vk"; [ $((rep % 2)) = 0 ] && order="vk cb1"
    for e in $order; do
      name=$tag-hw-$e-rep$rep
      bench "$OUT/$name.log" "${hw[@]}" --engines $e --frames "$FRAMES" --throughput "$THROUGHPUT" \
        --json "$OUT/$name.json" "$clip" || echo "   FAIL  $name, see $OUT/$name.log"
      grep -E "^(cb1|yblod-vulkan) " "$OUT/$name.log" | sed "s/^/   rep$rep /"
      grep -E "^== after" "$OUT/$name.log" | sed "s/^== after: /         after: /"
    done
  done

  # 3. Options, per picture (each its own process; one run, --frames pictures).
  #    name | environment | dvbench options
  while IFS='|' read -r oname oenv oargs; do
    [ -n "$oname" ] || continue
    name=$tag-opt-$oname
    env $oenv "$BENCH" ${NODE:+--node $NODE} "${hw[@]}" --frames "$FRAMES" $oargs --json "$OUT/$name.json" "$clip" \
      >"$OUT/$name.log" 2>&1 || echo "   FAIL  option $oname, see $OUT/$name.log"
    printf '   %-22s %s\n' "$oname" "$(grep -E "per picture" "$OUT/$name.log" | sed 's/.*per picture: //' | head -1)"
  done <<'OPTIONS'
vk-annexb||--engines vk
vk-generic|DVVK_SPECIALIZE=0|--engines vk
vk-wide|DVVK_NARROW=0|--engines vk
vk-no-tile|DVVK_ANNEXB_TILE=0|--engines vk
vk-one-row|DVVK_PACK_ONE_ROW=1|--engines vk
vk-bilinear||--engines vk --el-scaler bilinear
vk-bilinear-one-row|DVVK_PACK_ONE_ROW=1|--engines vk --el-scaler bilinear
vk-bilinear-generic|DVVK_SPECIALIZE=0|--engines vk --el-scaler bilinear
cb1-master||--engines cb1
cb1-ref-bilinear||--engines cb1 --cb1-mode reference --cb1-fel bilinear
OPTIONS

  # 3b. Real-time playback: Master Reference and the bilinear compromise.
  if [ "$PLAY" != 0 ]; then
    while IFS='|' read -r pname pargs; do
      [ -n "$pname" ] || continue
      name=$tag-play-$pname
      bench "$OUT/$name.log" "${hw[@]}" $pargs --frames 1 --warmup 0 --play "$PLAY" \
        --json "$OUT/$name.json" "$clip" || echo "   FAIL  $name, see $OUT/$name.log"
      grep -E "play:" "$OUT/$name.log" | sed "s/^/   $pname  /"
    done <<'PLAYS'
cb1|--engines cb1
vk|--engines vk
vk-bilinear|--engines vk --el-scaler bilinear
cb1-ref-bilinear|--engines cb1 --cb1-mode reference --cb1-fel bilinear
PLAYS
  fi

  # 4. Where the Vulkan engine's GPU time goes (GPU timestamps, reported every
  # 120 frames of the throughput run).
  DVVK_PROFILE=1 DVVK_PROFILE_WINDOW=24 bench "$OUT/$tag-vk-profile.log" "${hw[@]}" --engines vk \
    --frames "$FRAMES" --throughput 15 "$clip"
  grep "^dvvk profile" "$OUT/$tag-vk-profile.log" | tail -2 | sed 's/^/   /'

  # The V3D compiler's view of every shader (caches off so all compile).
  cache=$(mktemp -d)
  V3D_DEBUG=shaderdb DVVK_PIPELINE_PROFILE=1 MESA_SHADER_CACHE_DISABLE=true XDG_CACHE_HOME="$cache" \
    bench "$OUT/$tag-shaderdb.log" "${hw[@]}" --frames 2 --warmup 0 "$clip"
  rm -rf "$cache"
  python3 "$HERE/shader-stats.py" "$OUT/$tag-shaderdb.log" >"$OUT/$tag-shader-stats.txt"
  echo "   shaders (instructions, threads, spills): $OUT/$tag-shader-stats.txt"
  grep -E "SPILLS|1 thread|2 threads" "$OUT/$tag-shader-stats.txt" | head -8 | sed 's/^/     /'

  # 5. Software decoding, for reference (once, both engines).
  if [ $SW = 1 ]; then
    for layout in sand p010; do
      bench "$OUT/$tag-sw-$layout.log" --source sw --layout $layout --frames "$FRAMES" --throughput "$THROUGHPUT" \
        --json "$OUT/$tag-sw-$layout.json" "$clip"
      grep -E "^(cb1 |yblod)" "$OUT/$tag-sw-$layout.log" | sed "s/^/   sw $layout  /"
    done
  fi
done | tee "$OUT/summary.txt"
echo "== end: $(snapshot)" >>"$OUT/system.txt"
python3 "$HERE/summarize.py" "$OUT" | tee "$OUT/table.txt"
echo "results: $OUT"
