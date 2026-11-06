#!/bin/bash
# dvbench + probe-dmabuf, aarch64, into $PREFIX/bin. Runs inside the build container.
set -euo pipefail
W=${WORK:-/work}   # this repo: /work in the build container, the checkout natively
cmake -S $W/bench -B $W/build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCB1_SOURCE_DIR=$W/third_party/cb1/CB1/src -DDVVK_INCLUDE_DIR=$W/third_party/yblod/yblod-engine/vk/include
cmake --build $W/build/bench
cmake --install $W/build/bench >/dev/null
