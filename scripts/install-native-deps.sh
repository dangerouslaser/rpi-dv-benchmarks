#!/bin/bash
# Build natively on an aarch64 Debian trixie / Raspberry Pi OS trixie machine
# (e.g. the Pi itself) instead of in Docker: the same packages as the build
# container. Then ./scripts/build-all.sh as usual (scripts/in-docker.sh runs
# natively).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ "$(uname -m)" = aarch64 ] || { echo "aarch64 only"; exit 1; }
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $(grep -v '^#' "$ROOT/docker/packages.txt")
