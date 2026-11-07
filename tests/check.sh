#!/bin/bash
# The engine's regression check: every case in tests/golden/hashes.txt whose
# clip is in clips/, run with software decode into SAND128, must reproduce its
# per-picture output hashes exactly. The golden hashes were verified identical
# to yblod's Vulkan engine (bilinear FEL) sample for sample (see that file).
#   scripts/vm.sh tests/check.sh            (or on the Pi / any aarch64 Linux)
#   tests/check.sh --mmr split              (any exact MMR mode must match)
#   tests/check.sh --validate               (also: the Khronos validation layer,
#                                            tests/vulkan/vk_layer_settings.txt;
#                                            any validation error fails)
# Exit 1 on any difference. dvbench must be on PATH (the VM's is).
set -u
cd "$(dirname "$0")/.." || exit 2
VALIDATE=0 MMR=exact
while [ $# -gt 0 ]; do
  case "$1" in
    --validate) VALIDATE=1 ;;
    --mmr) MMR=$2; shift ;;
    *) echo "usage: tests/check.sh [--mmr exact|split|exact64] [--validate]"; exit 2 ;;
  esac
  shift
done
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
