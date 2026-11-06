#!/bin/bash
# Derive single-layer test clips from a Profile 7 FEL clip, so the engine's
# other routes get exercised (the FEL clips only take the dual-layer route):
#   p81-NAME.mkv  Profile 8.1 (dovi_tool mode 2): polynomial reshape, route 2
#   mmr-NAME.mkv  BL + RPU with the MMR mapping kept (mode 1, EL dropped):
#                 the piecewise reshape, route 3 (taken with --yblod)
# Needs ffmpeg, dovi_tool and mkvmerge on the host (brew install ffmpeg
# dovi_tool mkvtoolnix). First SECONDS (6) of each source.
#   tests/make-test-clips.sh clips/SOME_P7_FEL.mkv [...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for tool in ffmpeg dovi_tool mkvmerge; do command -v $tool >/dev/null || { echo "missing $tool"; exit 1; }; done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for src in "$@"; do
  n=$(basename "$src" | sed 's/\.[^.]*$//' | tr -c 'A-Za-z0-9\n' _ | cut -c1-24)
  ffmpeg -v error -y -t "${SECONDS_:-6}" -i "$src" -map 0:v:0 -c:v copy -bsf:v hevc_mp4toannexb -f hevc "$tmp/$n.hevc"
  dovi_tool -m 2 convert --discard "$tmp/$n.hevc" -o "$tmp/$n-p81.hevc" >/dev/null
  dovi_tool -m 1 convert --discard "$tmp/$n.hevc" -o "$tmp/$n-mmr.hevc" >/dev/null
  mkvmerge -q -o "$ROOT/clips/p81-$n.mkv" "$tmp/$n-p81.hevc"
  mkvmerge -q -o "$ROOT/clips/mmr-$n.mkv" "$tmp/$n-mmr.hevc"
  echo "clips/p81-$n.mkv clips/mmr-$n.mkv"
done
