#!/bin/bash
# The Pi-path checks, on the aarch64 build, in the test VM (or natively on
# aarch64 Linux): every one requires byte-identical output.
#   scripts/vm.sh tests/check.sh [CLIP...]      (default: every file in clips/)
#     1. dvvk-int64-check: the emulated 64-bit shader helpers vs native int64
#     2. per clip, the Vulkan engine (CB1 contract, then yblod's own routes):
#        linear P010 input == SAND128 input == SAND + no int64 + no push
#        descriptors (the Pi 5 configuration)
#     3. per clip, CB1 == the Vulkan engine (SAND input, CB1 contract)
# FRAMES (default 8) pictures each. Exit 1 on any difference.
set -u
cd "$(dirname "$0")/.."
FRAMES=${FRAMES:-8}
WORK=${WORK:-build/check}
mkdir -p "$WORK"
CLIPS=("$@")
[ ${#CLIPS[@]} -gt 0 ] || CLIPS=(clips/*)
[ -e "${CLIPS[0]}" ] || { echo "no clips: put Dolby Vision files in clips/ (docs/SETUP.md)"; exit 2; }
fail=0
hashes() { # out, args...: the "hash ENGINE N VALUE" lines of one run
  local out=$1; shift
  dvbench --frames "$FRAMES" --warmup 0 --hash "$@" 2>"$out.err" | grep '^hash' >"$out"
}
same() { # label, files...: all non-empty and identical (engine names ignored)
  local label=$1 first=$2; shift 2
  if [ ! -s "$first" ]; then echo "skip  $label (engine declined these pictures)"; return; fi
  for f in "$@"; do
    if ! cmp -s <(cut -d' ' -f3- "$first") <(cut -d' ' -f3- "$f"); then
      echo "FAIL  $label: $(basename "$f") differs (see $WORK)"; fail=1; return
    fi
  done
  echo "ok    $label ($(wc -l <"$first") pictures)"
}

echo "== emulated 64-bit helpers vs native int64"
dvvk-int64-check - 16 2>/dev/null | sed 's/^/   /'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1

for clip in "${CLIPS[@]}"; do
  t=$WORK/$(basename "$clip" | tr -c 'A-Za-z0-9\n' _ | cut -c1-40)
  echo "== $clip"
  for mode in cb1 yblod; do
    extra=; [ $mode = yblod ] && extra=--yblod
    hashes "$t-$mode-p010" --engines vk --layout p010 $extra "$clip"
    hashes "$t-$mode-sand" --engines vk --layout sand $extra "$clip"
    DVVK_NO_INT64=1 DVVK_NO_PUSH_DESCRIPTOR=1 hashes "$t-$mode-pi5" --engines vk --layout sand $extra "$clip"
    same "vulkan $mode: P010 == SAND == Pi 5 mode" "$t-$mode-p010" "$t-$mode-sand" "$t-$mode-pi5"
  done
  hashes "$t-cb1" --engines cb1 --layout sand "$clip"
  same "CB1 == vulkan (SAND, CB1 contract)" "$t-cb1-sand" "$t-cb1"
done
[ $fail = 0 ] && echo "all checks passed" || echo "CHECKS FAILED"
exit $fail
