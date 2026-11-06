#!/bin/bash
# Run a command in the arm64 build container with this repo at /work.
#   scripts/in-docker.sh scripts/build-ffmpeg.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
docker image inspect rpi5-dv-build >/dev/null 2>&1 || docker build -t rpi5-dv-build "$ROOT/docker"
mkdir -p "$ROOT/build/prefix"
exec docker run --rm ${DOCKER_TTY:-} -v "$ROOT":/work -v "$ROOT/build/prefix":/opt/dv \
  -e DV_THREADS rpi5-dv-build "$@"
