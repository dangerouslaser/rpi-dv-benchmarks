#!/bin/bash
# On the Raspberry Pi 5: probe the system, then benchmark yblod's Vulkan engine
# against CB1 (GLES) on every clip given, from the Pi's hardware HEVC decoder
# (V4L2 request, SAND128) and, for reference, from software decoding.
#
#   ./run-bench.sh [options] CLIP...
#     --frames N        pictures per clip (default 48)
#     --throughput S    seconds of flat-out rendering per engine (default 20)
#     --no-sw           skip the software-decode reference runs
#     --out DIR         results directory (default results/HOST-DATE)
#
# Writes DIR/system.txt, one log and one JSON per run, and DIR/summary.txt.
# Send the whole directory back.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
BENCH="$HERE/bin/dvbench"
FRAMES=48 THROUGHPUT=20 SW=1 OUT=
CLIPS=()
while [ $# -gt 0 ]; do
  case $1 in
    --frames) FRAMES=$2; shift ;;
    --throughput) THROUGHPUT=$2; shift ;;
    --no-sw) SW=0 ;;
    --out) OUT=$2; shift ;;
    -*) echo "unknown option $1"; exit 2 ;;
    *) CLIPS+=("$1") ;;
  esac
  shift
done
[ ${#CLIPS[@]} -gt 0 ] || { sed -n 2,13p "$0"; exit 2; }
OUT=${OUT:-results/$(hostname)-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"

# The V3D render node (Vulkan and GLES both run there; vc4 is display only).
NODE=
for n in /sys/class/drm/renderD*; do
  [ "$(basename "$(readlink -f "$n/device/driver")")" = v3d ] && NODE=/dev/dri/$(basename "$n")
done

{
  echo "== dvbench bundle"; cat "$HERE/VERSION" 2>/dev/null
  echo "== board"; tr -d '\0' </proc/device-tree/model 2>/dev/null; echo; uname -a
  grep -E "^(PRETTY_NAME|VERSION)=" /etc/os-release
  echo "== memory"; free -m
  echo "== v3d render node: ${NODE:-NOT FOUND}"; ls -l /dev/dri
  echo "== clocks"; for c in arm core v3d h264 isp hevc; do printf "%s " $c; vcgencmd measure_clock $c 2>/dev/null || echo; done
  vcgencmd get_throttled 2>/dev/null; vcgencmd measure_temp 2>/dev/null
  echo "== cpufreq governor"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
  echo "== dmabuf allocators"; ls -l /dev/udmabuf /dev/dma_heap 2>&1
  echo "== V4L2 decoders"; v4l2-ctl --list-devices 2>&1 || echo "(v4l2-ctl not installed: apt install v4l-utils)"
  echo "== Vulkan"; vulkaninfo --summary 2>&1 || echo "(vulkaninfo not installed: apt install vulkan-tools)"
  echo "== Vulkan features the engine cares about"
  vulkaninfo 2>/dev/null | grep -E "shaderInt64|shaderInt16|storageBuffer16BitAccess|storageBuffer8BitAccess|timelineSemaphore|synchronization2|maintenance4|VK_KHR_push_descriptor|VK_KHR_external_semaphore_fd|VK_EXT_external_memory_dma_buf|VK_EXT_image_drm_format_modifier|VK_EXT_queue_family_foreign|VK_EXT_physical_device_drm|subgroupSize |maxComputeWorkGroupInvocations|maxComputeSharedMemorySize" | sort -u
  echo "== EGL/GLES"; eglinfo -B 2>&1 | head -40 || echo "(eglinfo not installed: apt install mesa-utils-bin)"
  echo "== dmabuf import probe"; "$HERE/bin/probe-dmabuf" 2>&1
} >"$OUT/system.txt" 2>&1
echo "system: $OUT/system.txt (v3d node ${NODE:-NOT FOUND})"

run() { # name, args...
  local name=$1; shift
  echo "-- $name"
  "$BENCH" --frames "$FRAMES" --throughput "$THROUGHPUT" ${NODE:+--node $NODE} --json "$OUT/$name.json" "$@" \
    >"$OUT/$name.log" 2>&1
  local rc=$?
  grep -E "^(source|decode|cb1 |yblod)" "$OUT/$name.log" | sed 's/^/   /'
  [ $rc = 0 ] || echo "   FAILED ($rc), see $OUT/$name.log"
  return $rc
}
check() { # name, args...: both engines' output hashes, picture by picture
  local name=$1; shift
  "$BENCH" --frames 12 --warmup 0 --hash ${NODE:+--node $NODE} "$@" >"$OUT/$name.log" 2>&1
  local cb=$(grep -c "^hash cb1 " "$OUT/$name.log") same=0
  for n in $(grep "^hash cb1 " "$OUT/$name.log" | cut -d' ' -f3); do
    a=$(grep "^hash cb1 $n " "$OUT/$name.log" | cut -d' ' -f4)
    b=$(grep "^hash yblod-vulkan $n " "$OUT/$name.log" | cut -d' ' -f4)
    [ -n "$a" ] && [ "$a" = "$b" ] && same=$((same + 1))
  done
  echo "   CB1 vs Vulkan output: $same of $cb pictures byte-identical"
}

i=0
for clip in "${CLIPS[@]}"; do
  i=$((i + 1))
  tag=$(printf "%02d" $i)-$(basename "$clip" | tr -c 'A-Za-z0-9\n' _ | cut -c1-32)
  echo "== $clip"
  elsw=
  if ! run "$tag-v4l2" --source v4l2 "$clip"; then
    # Both layers on the one HEVC block failed: the EL in software instead.
    run "$tag-v4l2-elsw" --source v4l2 --el-sw "$clip" && elsw=--el-sw
  fi
  check "$tag-v4l2-hash" --source v4l2 $elsw "$clip"
  # Where the Vulkan engine's GPU time goes, stage by stage (GPU timestamps;
  # reported every PROFILE_WINDOW frames of the throughput run).
  DVVK_PROFILE=1 "$BENCH" --engines vk --frames 24 --throughput 15 ${NODE:+--node $NODE} --source v4l2 $elsw \
    "$clip" >"$OUT/$tag-vk-profile.log" 2>&1
  grep "^dvvk profile" "$OUT/$tag-vk-profile.log" | tail -2 | sed 's/^/   /'
  [ $SW = 1 ] && run "$tag-sw-sand" --source sw --layout sand "$clip"
done | tee "$OUT/summary.txt"
echo "results: $OUT"
