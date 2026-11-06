#!/bin/bash
# yblod's Vulkan engine (libdv_engine_vk.so.1) and its CPU tests, aarch64.
# Runs inside the build container. VK_SRC overrides the crate (our port).
set -euo pipefail
VK_SRC=${VK_SRC:-/work/third_party/yblod/yblod-engine/vk}
export YBLOD_ENGINE_SRC=${YBLOD_ENGINE_SRC:-/work/third_party/yblod/yblod-engine/src}
export FFMPEG_INCLUDE=$PREFIX/include
export CARGO_TARGET_DIR=/work/build/vk
cd "$VK_SRC"
cargo build --release --offline 2>/dev/null || cargo build --release
cargo test --release ${CARGO_TEST_ARGS:-}
install -m755 $CARGO_TARGET_DIR/release/libdv_engine_vk.so "$PREFIX/lib/libdv_engine_vk.so.1"
