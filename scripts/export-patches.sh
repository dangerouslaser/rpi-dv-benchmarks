#!/bin/bash
# Engine work happens on branch rpi5 in third_party/<name>; this records it as
# patches/<name>/*.patch (what fetch-sources.sh applies on a fresh checkout).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
for name in ${@:-yblod}; do
  pin=$(awk -v n="$name" '$1==n {print $3}' "$ROOT/sources.lock")
  rm -rf "$ROOT/patches/$name"; mkdir -p "$ROOT/patches/$name"
  git -C "$ROOT/third_party/$name" format-patch -q --no-signature -o "$ROOT/patches/$name" "$pin"..rpi5
  ls "$ROOT/patches/$name"
done
