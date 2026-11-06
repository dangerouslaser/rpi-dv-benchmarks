#!/bin/bash
# Build natively on an aarch64 Debian trixie / Raspberry Pi OS trixie machine
# (e.g. the Pi itself) instead of in Docker: the same packages as the build
# container, plus Rust from rustup (Debian's is too old for the engine).
# Then ./scripts/build-all.sh as usual (scripts/in-docker.sh runs natively).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ "$(uname -m)" = aarch64 ] || { echo "aarch64 only"; exit 1; }
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $(grep -v '^#' "$ROOT/docker/packages.txt")
if ! command -v cargo >/dev/null; then
  curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable
  echo 'Rust installed: run  . "$HOME/.cargo/env"  (or open a new shell) before building'
fi
