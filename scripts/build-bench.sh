#!/bin/bash
# dvbench + probe-dmabuf, aarch64, into $PREFIX/bin. Runs inside the build container.
set -euo pipefail
cmake -S /work/bench -B /work/build/bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCB1_SOURCE_DIR=/work/third_party/cb1/CB1/src -DDVVK_INCLUDE_DIR=/work/third_party/yblod/yblod-engine/vk/include
cmake --build /work/build/bench
cmake --install /work/build/bench >/dev/null
