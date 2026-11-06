#!/bin/bash
# Run a build command for aarch64:
#   - on macOS/x86 hosts: in the arm64 build container, this repo at /work,
#     build/prefix at /opt/dv;
#   - on an aarch64 Linux host (e.g. the Pi), or with DV_NATIVE=1: natively,
#     with the same variables pointing into this checkout
#     (scripts/install-native-deps.sh first).
#   scripts/in-docker.sh bash scripts/build-ffmpeg.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/build/prefix"
if [ "${DV_NATIVE:-}" = 1 ] || { [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] && [ "${DV_NATIVE:-}" != 0 ]; }; then
  P=$ROOT/build/prefix
  cd "$ROOT"
  exec env WORK="$ROOT" PREFIX="$P" \
    PKG_CONFIG_PATH="$P/lib/pkgconfig:$P/lib/aarch64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    LD_LIBRARY_PATH="$P/lib:$P/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    PATH="$HOME/.cargo/bin:$PATH" "$@"
fi
docker image inspect rpi5-dv-build >/dev/null 2>&1 || docker build -t rpi5-dv-build "$ROOT/docker"
exec docker run --rm ${DOCKER_TTY:-} -v "$ROOT":/work -v "$ROOT/build/prefix":/opt/dv \
  -e DV_THREADS ${DOCKER_ENV:-} rpi5-dv-build "$@"
