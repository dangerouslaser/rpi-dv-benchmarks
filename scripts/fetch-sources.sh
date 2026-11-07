#!/bin/bash
# Fetch the pinned inputs in sources.lock into third_party/ (idempotent).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP=$ROOT/third_party
mkdir -p "$TP"

while read -r name url pin; do
  case "$name" in ''|'#'*) continue ;; esac
  dest=$TP/$name
  if [[ $url == *.git ]]; then
    if [ -d "$dest/.git" ] && git -C "$dest" merge-base --is-ancestor "$pin" HEAD 2>/dev/null; then
      echo "ok      $name @ ${pin:0:10}"; continue
    fi
    rm -rf "$dest"; mkdir -p "$dest"
    git -C "$dest" init -q
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch -q --depth 1 origin "$pin"
    git -C "$dest" -c advice.detachedHead=false checkout -q FETCH_HEAD
    if ls "$ROOT/patches/$name"/*.patch >/dev/null 2>&1; then
      git -C "$dest" checkout -q -b rpi5
      git -C "$dest" -c user.name=rpi5-dv -c user.email=rpi5-dv@localhost am -q --committer-date-is-author-date "$ROOT/patches/$name"/*.patch
    fi
    echo "fetched $name @ ${pin:0:10} (+$(ls "$ROOT/patches/$name"/*.patch 2>/dev/null | wc -l | tr -d ' ') patches)"
  else
    archive=$TP/$(basename "$url")
    if [ ! -f "$archive" ] || ! echo "$pin  $archive" | shasum -a 256 -c - >/dev/null 2>&1; then
      curl -fsSL -o "$archive" "$url"
      echo "$pin  $archive" | shasum -a 256 -c - >/dev/null
    fi
    echo "ok      $name archive"
  fi
done <"$ROOT/sources.lock"
