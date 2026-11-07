#!/bin/bash
# Fetch the pinned inputs in sources.lock into third_party/ (idempotent): each
# is an archive, checked against its sha256.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP=$ROOT/third_party
mkdir -p "$TP"

while read -r name url pin; do
  case "$name" in ''|'#'*) continue ;; esac
  archive=$TP/$(basename "$url")
  if [ ! -f "$archive" ] || ! echo "$pin  $archive" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fsSL -o "$archive" "$url"
    echo "$pin  $archive" | shasum -a 256 -c - >/dev/null
  fi
  echo "ok      $name archive"
done <"$ROOT/sources.lock"
