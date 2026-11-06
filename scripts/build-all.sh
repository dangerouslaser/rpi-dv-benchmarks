#!/bin/bash
# Everything, aarch64, in the build container: FFmpeg (LibreELEC RPi5 config +
# DV patches), libplacebo + CB1, yblod's Vulkan engine, dvbench; then the Pi
# bundle in out/. Logs in build/*.log.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build
./scripts/fetch-sources.sh
for step in ffmpeg cb1 vk bench; do
  printf '%-8s' "$step"
  if scripts/in-docker.sh bash "scripts/build-$step.sh" >"build/$step.log" 2>&1; then
    echo ok
  else
    echo "FAILED (build/$step.log)"; tail -20 "build/$step.log"; exit 1
  fi
done
echo "prefix: build/prefix (aarch64)"
./scripts/make-bundle.sh
