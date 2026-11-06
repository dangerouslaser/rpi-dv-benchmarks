#!/bin/bash
# Run a command in the test VM, in this repo, with the aarch64 prefix.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec limactl shell --workdir "$ROOT" rpi5dv -- env \
  LD_LIBRARY_PATH="$ROOT/build/prefix/lib" PREFIX="$ROOT/build/prefix" "$@"
