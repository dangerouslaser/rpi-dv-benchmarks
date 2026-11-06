#!/bin/bash
# Run a command against the aarch64 build: in the test VM (macOS), or natively
# on an aarch64 Linux host. In this repo's directory, with build/prefix on
# PATH and LD_LIBRARY_PATH.
#   scripts/vm.sh dvbench --hash clips/X.mkv
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=$ROOT/build/prefix
if [ "$(uname -s)" = Linux ]; then
  cd "$ROOT" && exec env PATH="$P/bin:$PATH" LD_LIBRARY_PATH="$P/lib" PREFIX="$P" "$@"
fi
exec limactl shell --workdir "$ROOT" "${VM_NAME:-rpi5dv}" -- env \
  PATH="$P/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  LD_LIBRARY_PATH="$P/lib" PREFIX="$P" "$@"
