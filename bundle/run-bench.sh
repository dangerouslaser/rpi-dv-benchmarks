#!/bin/bash
# On the Raspberry Pi 5: probe the system, check the engine's output, then
# measure it on every clip given, from the Pi's hardware HEVC decoder (V4L2
# request, SAND128).
#
#   ./run-bench.sh [options] CLIP...
#     --frames N        pictures per run (default 12: the decoder's 32-buffer
#                       pool limits hardware-decode runs to 12 held pictures)
#     --throughput S    seconds of flat-out rendering per run (default 10)
#     --repeat N        timed runs per clip (default 3)
#     --play S          seconds of real-time playback (default 60; 0 skips)
#     --start S         seek each clip to S seconds first (default 0)
#     --filters F       cb1 or yblod for every clip (see dvbench). Default: per
#                       clip, cb1, or yblod when the engine refuses the clip's
#                       plan under CB1's contract (Profile 8.1 MMR single-layer
#                       clips, e.g. tests/make-test-clips.sh's p81mmr-*)
#     --quick           correctness only
#     --out DIR         results directory (default results/HOST-DATE)
#
# Per clip:
#   1. correctness: the output from hardware decoding == from software decoding
#      of the same pictures (HEVC decoding is exact: identical output proves the
#      SAND input handling end to end), and every exact MMR mode identical
#   2. performance: REPEAT timed runs (per picture, and flat-out throughput),
#      clocks / temperature / throttling recorded around every run
#   3. MMR modes and passes: exact, split, float; per-pass GPU times
#   4. real-time playback: decoding on its own thread, rendering on the
#      stream's frame clock; pictures that missed their display deadline
#   5. the V3D compiler's statistics for the engine's shaders
# Writes DIR/system.txt, a log and JSON per run and DIR/summary.txt (medians
# over repeats). Send the whole directory back. Exits 1 if any run failed or
# any correctness check differed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BENCH="$HERE/bin/dvbench"
FRAMES=12 THROUGHPUT=10 REPEAT=3 PLAY=60 START=0 FILTERS='' QUICK=0 OUT=''
CLIPS=()
while [ $# -gt 0 ]; do
  case $1 in
    --frames) FRAMES=$2; shift ;;
    --throughput) THROUGHPUT=$2; shift ;;
    --repeat) REPEAT=$2; shift ;;
    --play) PLAY=$2; shift ;;
    --start) START=$2; shift ;;
    --filters) FILTERS=$2; shift ;;
    --quick) QUICK=1 ;;
    --out) OUT=$2; shift ;;
    -*) echo "unknown option $1"; exit 2 ;;
    *) CLIPS+=("$1") ;;
  esac
  shift
done
[ ${#CLIPS[@]} -gt 0 ] || { sed -n 2,31p "$0"; exit 2; }
OUT=${OUT:-results/$(hostname)-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

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

snapshot() { # one line: temperature, throttle flags, clocks
  local t th c=
  t=$(vcgencmd measure_temp 2>/dev/null | cut -d= -f2)
  th=$(vcgencmd get_throttled 2>/dev/null | cut -d= -f2)
  for k in arm core v3d hevc; do c="$c $k=$(vcgencmd measure_clock $k 2>/dev/null | cut -d= -f2)"; done
  echo "temp=$t throttled=$th$c"
}

{
  echo "== dvbench bundle"; cat "$HERE/VERSION" 2>/dev/null
  echo "== board"; tr -d '\0' </proc/device-tree/model 2>/dev/null; echo; uname -a
  grep -E "^(PRETTY_NAME|VERSION)=" /etc/os-release
  echo "== memory"; free -m
  echo "== $GOV_NOTE"
  echo "== $CMA_NOTE"
  echo "== start: $(snapshot)"
  echo "== GPU clock setting: $(vcgencmd get_config v3d_freq 2>/dev/null)"
  echo "== cooling"; ls -d /sys/devices/platform/cooling_fan* 2>/dev/null || echo "no fan device found"
  echo "== dmabuf allocators"; ls -l /dev/udmabuf /dev/dma_heap 2>&1
  echo "== V4L2 decoders"; v4l2-ctl --list-devices 2>&1 || echo "(v4l2-ctl not installed: apt install v4l-utils)"
  echo "== Mesa packages"; dpkg -l 'mesa-vulkan-drivers' 2>/dev/null | grep ^ii
  echo "== Vulkan"; vulkaninfo --summary 2>&1 || echo "(vulkaninfo not installed: apt install vulkan-tools)"
  echo "== dmabuf and engine probe"; "$HERE/bin/dvbench" --probe 2>&1
} >"$OUT/system.txt" 2>&1
echo "system: $OUT/system.txt ($GOV_NOTE)"

bench() { # log, args...: one dvbench run, snapshots around it in the log
  local log=$1; shift
  { echo "== before: $(snapshot)"
    "$BENCH" --filters "$CF" "$@"
    local rc=$?
    echo "== after: $(snapshot)"
    echo "== exit $rc"
    return $rc; } >"$log" 2>&1
}
hashes() { grep "^hash " "$1" | cut -d' ' -f2,3; }
same() { # label, a, b: picture-by-picture identity of two "N HASH" lists
  local n
  n=$(echo "$2" | grep -c .)
  if [ "$n" -gt 0 ] && [ "$2" = "$3" ]; then echo "   ok    $1: $n of $n pictures identical"
  else echo "   FAIL  $1: outputs differ (or a run failed)"; return 1; fi
}
refused() { # log: the planner or the engine refused the clip (PI_E_UNSUPPORTED_PLAN / PI_E_BAD_RPU)
  grep -qE "^pi: (render refused|plan outside this engine|the planner refuses)" "$1"
}

i=0 fail=0
{ for clip in "${CLIPS[@]}"; do
  i=$((i + 1))
  tag=$(printf '%02d' $i)-$(basename "$clip" | sed 's/\.[^.]*$//; s/[^A-Za-z0-9]/_/g' | cut -c1-28)
  hw=(--source v4l2 --start "$START")
  echo "== $clip"
  # 1. correctness; with no --filters, CB1's filters unless the engine
  # refuses the clip's plan under CB1's contract, then yblod's
  CF=${FILTERS:-cb1}
  bench "$OUT/$tag-check-hw.log" "${hw[@]}" --frames "$FRAMES" --warmup 0 --hash "$clip"
  rc=$?
  if [ -z "$FILTERS" ] && refused "$OUT/$tag-check-hw.log"; then
    mv "$OUT/$tag-check-hw.log" "$OUT/$tag-check-hw-cb1.log"
    CF=yblod
    bench "$OUT/$tag-check-hw.log" "${hw[@]}" --frames "$FRAMES" --warmup 0 --hash "$clip"
    rc=$?
    echo "   filters: yblod (CB1's contract refused the plan: $tag-check-hw-cb1.log)"
  else
    echo "   filters: $CF"
  fi
  [ $rc = 0 ] || { echo "   FAIL  dvbench exit $rc: $OUT/$tag-check-hw.log"; fail=1; }
  grep -E "^(source|decode):" "$OUT/$tag-check-hw.log" | sed 's/^/   /'
  bench "$OUT/$tag-check-sw.log" --source sw --start "$START" --frames "$FRAMES" --warmup 0 --hash "$clip" || fail=1
  same "hardware decode == software decode (the SAND input end to end)" \
    "$(hashes "$OUT/$tag-check-hw.log")" "$(hashes "$OUT/$tag-check-sw.log")" || fail=1
  for m in split exact64; do
    bench "$OUT/$tag-check-$m.log" "${hw[@]}" --frames "$FRAMES" --warmup 0 --hash --mmr $m "$clip" || fail=1
    same "MMR exact == $m" "$(hashes "$OUT/$tag-check-hw.log")" "$(hashes "$OUT/$tag-check-$m.log")" || fail=1
  done
  [ $QUICK = 1 ] && continue
  # 2. performance
  for rep in $(seq 1 "$REPEAT"); do
    bench "$OUT/$tag-run$rep.log" "${hw[@]}" --frames "$FRAMES" --warmup 2 --throughput "$THROUGHPUT" \
      --json "$OUT/$tag-run$rep.json" "$clip" || { echo "   FAIL  $OUT/$tag-run$rep.log"; fail=1; }
    grep -E "^(per picture|throughput):" "$OUT/$tag-run$rep.log" | sed "s/^/   run $rep: /"
  done
  # 3. MMR modes, per pass
  for m in exact split float; do
    (export PI_PROFILE=1; bench "$OUT/$tag-mode-$m.log" "${hw[@]}" --frames "$FRAMES" --warmup 2 --mmr $m \
      --json "$OUT/$tag-mode-$m.json" "$clip") || { echo "   FAIL  $OUT/$tag-mode-$m.log"; fail=1; }
    echo "   $m: $(grep -E "^per picture:" "$OUT/$tag-mode-$m.log" | cut -d, -f1 | cut -d: -f2-) |" \
      "$(grep -oE "P1 chroma [0-9.]+, P2 4:2:2 \+ pack [0-9.]+ ms" "$OUT/$tag-mode-$m.log")"
  done
  # 4. real-time playback
  if [ "$PLAY" != 0 ]; then
    bench "$OUT/$tag-play.log" "${hw[@]}" --frames 4 --warmup 0 --play "$PLAY" --json "$OUT/$tag-play.json" "$clip" ||
      { echo "   FAIL  $OUT/$tag-play.log"; fail=1; }
    grep -E "^play:" "$OUT/$tag-play.log" | sed 's/^/   /'
  fi
  # 5. shader statistics
  (export V3D_DEBUG=shaderdb; bench "$OUT/$tag-shaderdb.log" "${hw[@]}" --frames 2 --warmup 0 "$clip") ||
    { echo "   FAIL  $OUT/$tag-shaderdb.log"; fail=1; }
  grep -E "MESA_SHADER_FRAGMENT" "$OUT/$tag-shaderdb.log" | sed 's/.*shader: /   shader: /' | sort -u
done; exit $fail; } | tee "$OUT/summary.txt"
status=${PIPESTATUS[0]}
python3 "$HERE/summarize.py" "$OUT" | tee -a "$OUT/summary.txt"
[ "${PIPESTATUS[0]}" = 0 ] || status=1
echo "results: $OUT"
[ "$status" = 0 ] || echo "SOME RUNS FAILED (FAIL lines above; the logs in $OUT)"
exit "$status"
