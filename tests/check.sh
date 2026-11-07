#!/bin/bash
# The engine's regression check: every case in tests/golden/hashes.txt whose
# clip is in clips/, run with software decode into SAND128, must reproduce its
# per-picture output hashes exactly. The golden hashes were verified identical
# to yblod's Vulkan engine (bilinear FEL) sample for sample (see that file).
#   scripts/vm.sh tests/check.sh            (or on the Pi / any aarch64 Linux)
#   tests/check.sh --mmr split              (any exact MMR mode must match)
#   tests/check.sh --gpuav                  (also: GPU-assisted validation, which
#                                            reports out-of-bounds buffer reads
#                                            in the shaders; slow)
#   tests/check.sh --validate               (also: the Khronos validation layer,
#                                            tests/vulkan/vk_layer_settings.txt;
#                                            any validation error fails)
# Exit 1 on any difference. dvbench must be on PATH (the VM's is).
set -u
cd "$(dirname "$0")/.." || exit 2
VALIDATE=0 GPUAV=0 MMR=exact
while [ $# -gt 0 ]; do
  case "$1" in
    --validate) VALIDATE=1 ;;
    --gpuav) GPUAV=1 ;;
    --mmr) MMR=$2; shift ;;
    *) echo "usage: tests/check.sh [--mmr exact|split|exact64] [--validate] [--gpuav]"; exit 2 ;;
  esac
  shift
done
case "$MMR" in
  exact|split|exact64) ;;
  *) echo "tests/check.sh: --mmr $MMR: the golden hashes are exact; use exact, split or exact64"; exit 2 ;;
esac
GOLDEN=tests/golden/hashes.txt
WORK=${WORK:-build/check}
mkdir -p "$WORK"
fail=0 ran=0
# the cases: "clip | options", in file order
cases=$(grep -v '^#' "$GOLDEN" | awk -F' [|] ' '{print $1 " | " $2}' | awk '!seen[$0]++')
while IFS= read -r case; do
  clip=${case%% | *}
  opts=${case#* | }
  if [ ! -e "clips/$clip" ]; then
    echo "skip  $clip ($opts): not in clips/"
    continue
  fi
  want=$(grep -F "$clip | $opts | " "$GOLDEN" | awk -F' [|] ' '{print $3, $4}')
  n=$(echo "$want" | wc -l | tr -d ' ')
  # shellcheck disable=SC2086
  got=$(dvbench --frames "$n" --warmup 0 --hash --mmr "$MMR" $opts "clips/$clip" 2>"$WORK/last.err" |
        awk '/^hash/ {print $2, $3}')
  ran=$((ran + 1))
  if [ "$got" = "$want" ]; then
    echo "ok    $clip ${opts:-(cb1 filters)}: $n pictures identical"
  else
    echo "FAIL  $clip ${opts:-(cb1 filters)}: output differs (dvbench stderr: $WORK/last.err)"
    diff <(echo "$want") <(echo "$got") | head -6 | sed 's/^/      /'
    fail=1
  fi
done <<<"$cases"
# The validation run: the first golden clip in clips/ whose case takes CB1's
# filters (so both filter sets accept it).
clip=
# dvbench's command line: unknown values and malformed numbers are errors
# (exit 2), and --json escapes what it writes (a file name with a quote, a
# backslash and a newline).
cli_ok=1
for args in "--mmr exat" "--filters cb2" "--source hw" "--frames 12x" "--start -1" "--fel 2" "--measure nope"; do
  # shellcheck disable=SC2086
  dvbench $args clips/none.mkv >/dev/null 2>&1
  [ $? = 2 ] || { echo "FAIL  dvbench $args: not refused with exit 2"; cli_ok=0; }
done
jclip=$(grep -v '^#' "$GOLDEN" | awk -F' [|] ' '{print $1}' | while read -r c; do [ -e "clips/$c" ] && { echo "$c"; break; }; done)
if [ -n "$jclip" ]; then
  odd=$WORK/$'odd "name\\ with\nnewline'.mkv
  ln -sf "$PWD/clips/$jclip" "$odd"
  dvbench --frames 2 --warmup 0 --json "$WORK/odd.json" "$odd" >/dev/null 2>&1
  python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); assert j["file"]==sys.argv[2], j["file"]' \
    "$WORK/odd.json" "$odd" 2>/dev/null || { echo "FAIL  dvbench --json: not valid JSON, or the name not kept"; cli_ok=0; }
  rm -f "$odd"
fi
ran=$((ran + 1))
if [ $cli_ok = 1 ]; then echo "ok    dvbench: bad option values refused, --json escaped"; else fail=1; fi

if [ $GPUAV = 1 ]; then
  # out-of-bounds reads in the shaders (P2 #3), every route and filter set
  for c in "DV FEL Power Rangers Credits Test.mkv|" "DV FEL Power Rangers Credits Test.mkv|--filters phase" \
           "sol/p84-2160-100.hevc|" "sol/p84-2160-100.hevc|--filters phase" "sol/p5-1080-100.hevc|" \
           "p81mmr-cmv.mkv|--filters phase"; do
    clip=${c%%|*} opts=${c#*|}
    [ -e "clips/$clip" ] || { echo "skip  gpuav $clip: not in clips/"; continue; }
    log=$WORK/gpuav.log
    # shellcheck disable=SC2086
    VK_LAYER_SETTINGS_PATH=$PWD/tests/vulkan/gpuav VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
      dvbench --frames 2 --warmup 0 $opts "clips/$clip" >"$log" 2>&1
    v=$(grep -c "Validation Error" "$log")
    ran=$((ran + 1))
    if [ "$v" = 0 ] && grep -qi "gpu.assisted\|gpuav" "$log"; then
      echo "ok    gpuav $clip ${opts:-(cb1 filters)}: no out-of-bounds reads"
    else
      echo "FAIL  gpuav $clip ${opts:-(cb1 filters)}: $v errors (or GPU-AV inactive), see $log"; cp "$log" "$WORK/gpuav-fail.log"; fail=1
    fi
  done
fi

# The API's behaviour (dvbench --selftest: errors, validation, frame lifetime,
# import release, teardown), on an FEL MMR clip if present; with --validate
# under the validation layer.
st=""
for c in "DV FEL Power Rangers Credits Test.mkv" "DV FEL All Layers Test (Woman at 80s).mkv"; do
  [ -e "clips/$c" ] && { st=$c; break; }
done
if [ -n "$st" ]; then
  log=$WORK/selftest.log
  if [ $VALIDATE = 1 ]; then
    VK_LAYER_SETTINGS_PATH=$PWD/tests/vulkan VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
      dvbench --frames 8 --warmup 0 --mmr "$MMR" --selftest "clips/$st" >"$log" 2>&1
  else
    dvbench --frames 8 --warmup 0 --mmr "$MMR" --selftest "clips/$st" >"$log" 2>&1
  fi
  rc=$?
  ran=$((ran + 1))
  v=$(grep -c "Validation Error" "$log")
  if [ $rc = 0 ] && [ "$v" = 0 ]; then
    echo "ok    selftest: $(grep -E '^selftest: [0-9]+ of' "$log" | cut -d' ' -f2-)"
  else
    echo "FAIL  selftest (exit $rc, $v validation errors): $log"; grep "^selftest: FAIL" "$log" | sed 's/^/      /'; fail=1
  fi
fi
if [ $VALIDATE = 1 ]; then
  while IFS= read -r case; do
    if [ -z "${case#* | }" ] && [ -e "clips/${case%% | *}" ]; then clip=${case%% | *}; break; fi
  done <<<"$cases"
  if [ -z "$clip" ] && [ $ran -gt 0 ]; then
    echo "FAIL  validation: no golden clip with CB1's filters in clips/"; fail=1
  elif [ -z "$clip" ]; then
    echo "skip  validation: no golden clip in clips/"
  fi
fi
if [ -n "$clip" ]; then
  for opts in "" "--filters yblod"; do
    log=$WORK/validate${opts:+-yblod}.log
    # shellcheck disable=SC2086
    VK_LAYER_SETTINGS_PATH=$PWD/tests/vulkan VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
      dvbench --frames 3 --warmup 0 $opts "clips/$clip" >"$log" 2>&1
    v=$(grep -c "Validation Error" "$log")
    if grep -q "Validation Layer Active" "$log" && [ "$v" = 0 ]; then
      echo "ok    validation ${opts:-(cb1 filters)}: no errors"
    else
      echo "FAIL  validation ${opts:-(cb1 filters)}: $v errors (or the layer missing), see $log"; fail=1
    fi
  done
fi
[ $ran -gt 0 ] || { echo "no clips: put the test clips in clips/ (docs/SETUP.md)"; exit 2; }
[ $fail = 0 ] && echo "all $ran checks passed" || echo "CHECKS FAILED"
exit $fail
