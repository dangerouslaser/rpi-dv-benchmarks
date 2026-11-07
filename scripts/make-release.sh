#!/bin/bash
# out/pi-dv-engine-<version>.tar.xz: the source release a distribution
# packages (LibreELEC's PKG_URL / PKG_SHA256): the committed tree at HEAD,
# <version> from CMakeLists.txt's project(). Prints the SHA-256.
#   scripts/make-release.sh
# Builds with: cmake -DPI_BUILD_BENCH=OFF (libpi_engine.so, pi_engine.h,
# pi_engine.pc).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$(sed -n 's/^project(pi-dv-engine VERSION \([0-9.]*\) .*/\1/p' "$ROOT/CMakeLists.txt")
[ -n "$V" ] || { echo "no version in CMakeLists.txt's project()" >&2; exit 1; }
git -C "$ROOT" diff --quiet HEAD || echo "note: uncommitted changes are not in the release (HEAD is)" >&2
NAME=pi-dv-engine-$V
mkdir -p "$ROOT/out"
git -C "$ROOT" archive --format=tar --prefix="$NAME/" HEAD -- . ':(exclude).cache' | xz -9 >"$ROOT/out/$NAME.tar.xz"
SHA=$(if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi <"$ROOT/out/$NAME.tar.xz" | cut -d' ' -f1)
echo "$ROOT/out/$NAME.tar.xz ($(du -h "$ROOT/out/$NAME.tar.xz" | cut -f1))"
echo "PKG_VERSION=\"$V\""
echo "PKG_SHA256=\"$SHA\""
