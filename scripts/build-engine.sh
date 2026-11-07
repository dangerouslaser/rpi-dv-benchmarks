#!/bin/bash
# The engine and its tools (CMakeLists.txt: engine/, bench/), aarch64, into
# $PREFIX/bin. Runs inside the build container (or natively on aarch64 Linux).
set -euo pipefail
W=${WORK:-/work}   # this repo: /work in the build container, the checkout natively
cmake -S $W -B $W/build/engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DPI_BUILD_BENCH=ON
cmake --build $W/build/engine
cmake --install $W/build/engine >/dev/null
