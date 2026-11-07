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
# dovi_tool mkvtoolnix). Writes clips/. The environment sets the cut: the first
# SECONDS_ (default 6) of each source, from START (default 0); SECONDS_, as
# bash's own SECONDS is a timer.
#   tests/make-test-clips.sh [--kinds K,...] [--name NAME] SOURCE [...]
#     --kinds K,...  which clips to make, of p81, mmr, p81mmr (default all);
#                    applies to every source after it
#     --name NAME    NAME for the next source only (default: its file name,
#                    non-alphanumerics as _, cut to 24 characters)
# The golden cases' clips (tests/golden/hashes.txt), from their sources:
#   S="clips/DV FEL All Layers Test (Woman at 80s).mkv"
#   tests/make-test-clips.sh --kinds p81,mmr --name All_Layers_Te "$S" \
#     --kinds p81mmr --name al "$S"
#   S="clips/DV FEL Test DT DL P7 CMV4.0 4000nits v3.mp4"
#   tests/make-test-clips.sh --kinds p81,mmr "$S" --kinds p81mmr --name cmv "$S"
#   tests/make-test-clips.sh --kinds p81mmr --name pr "clips/DV FEL Power Rangers Credits Test.mkv"
#   tests/make-test-clips.sh --kinds p81,p81mmr --name 1917 1917_P7_SOURCE \
#     --kinds p81mmr --name dune2 DUNE2_P7_SOURCE
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
usage() { sed -n 2,27p "$0"; exit 2; }
[ $# -gt 0 ] || usage
for tool in ffmpeg dovi_tool mkvmerge; do command -v $tool >/dev/null || { echo "missing $tool"; exit 1; }; done
mkdir -p "$ROOT/clips"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
KINDS=p81,mmr,p81mmr NAME='' made=0
while [ $# -gt 0 ]; do
  case $1 in
    --kinds) [ $# -ge 2 ] || usage; KINDS=$2; shift 2; continue ;;
    --name) [ $# -ge 2 ] || usage; NAME=$2; shift 2; continue ;;
    -*) usage ;;
  esac
  src=$1; shift
  n=${NAME:-$(basename "$src" | sed 's/\.[^.]*$//' | tr -c 'A-Za-z0-9\n' _ | cut -c1-24)}
  NAME=
  ffmpeg -v error -y -ss "${START:-0}" -t "${SECONDS_:-6}" -i "$src" -map 0:v:0 -c:v copy -bsf:v hevc_mp4toannexb \
    -f hevc "$tmp/$n.hevc"
  for kind in ${KINDS//,/ }; do
    case $kind in
      p81) mode=2 ;;
      mmr) mode=1 ;;
      p81mmr) mode=5 ;;
      *) echo "unknown kind $kind (p81, mmr, p81mmr)"; exit 2 ;;
    esac
    dovi_tool -m "$mode" convert --discard "$tmp/$n.hevc" -o "$tmp/$n-$kind.hevc" >/dev/null
    mkvmerge -q -o "$ROOT/clips/$kind-$n.mkv" "$tmp/$n-$kind.hevc"
    echo "clips/$kind-$n.mkv"
  done
  made=$((made + 1))
done
[ $made -gt 0 ] || usage
