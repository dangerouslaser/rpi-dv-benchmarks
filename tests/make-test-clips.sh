#!/bin/bash
# Derive single-layer test clips from Profile 7 sources, so every route of the
# engine is exercised (FEL sources take the dual-layer route only):
#   p81-NAME.mkv     Profile 8.1, mapping replaced (dovi_tool mode 2):
#                    polynomial reshape, the single-layer route
#   mmr-NAME.mkv     BL + RPU, EL dropped (mode 1, MEL compatible)
#   p81mmr-NAME.mkv  Profile 8.1 keeping the mapping (mode 5): MMR chroma when
#                    the source has it, the general route (dvbench --filters
#                    yblod; CB1's contract doesn't take it)
# Needs ffmpeg, dovi_tool and mkvmerge on the host (brew install ffmpeg
# dovi_tool mkvtoolnix). First SECONDS (6) of each source, from START (0).
#   tests/make-test-clips.sh clips/SOME_P7.mkv [...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for tool in ffmpeg dovi_tool mkvmerge; do command -v $tool >/dev/null || { echo "missing $tool"; exit 1; }; done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for src in "$@"; do
  n=$(basename "$src" | sed 's/\.[^.]*$//' | tr -c 'A-Za-z0-9\n' _ | cut -c1-24)
  ffmpeg -v error -y -ss "${START:-0}" -t "${SECONDS_:-6}" -i "$src" -map 0:v:0 -c:v copy -bsf:v hevc_mp4toannexb \
    -f hevc "$tmp/$n.hevc"
  for m in "2 p81" "1 mmr" "5 p81mmr"; do
    set -- $m
    dovi_tool -m "$1" convert --discard "$tmp/$n.hevc" -o "$tmp/$n-$2.hevc" >/dev/null
    mkvmerge -q -o "$ROOT/clips/$2-$n.mkv" "$tmp/$n-$2.hevc"
    echo "clips/$2-$n.mkv"
  done
done
