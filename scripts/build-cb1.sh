#!/bin/bash
# libplacebo (pinned, CB1's patches, LibreELEC's options: OpenGL/GLES only)
# then CB1's dvbridge_core static library. Runs inside the build container.
set -euo pipefail
W=${WORK:-/work}   # this repo: /work in the build container, the checkout natively
TP=$W/third_party
SRC=$W/build/libplacebo-src
if [ ! -f "$SRC/.patched" ]; then
  rm -rf "$SRC"; cp -a $TP/libplacebo "$SRC"; rm -rf "$SRC/.git"
  git -C "$SRC" init -q   # meson's version script wants a git tree or VERSION
  for p in $TP/cb1/CB1/patches/libplacebo/*.patch; do
    echo "patch $(basename "$p")"
    patch -d "$SRC" -p1 --forward --quiet <"$p"
  done
  touch "$SRC/.patched"
fi
if [ ! -f $W/build/libplacebo/build.ninja ]; then
  meson setup $W/build/libplacebo "$SRC" --prefix="$PREFIX" --libdir=lib --buildtype=release \
    -Ddefault_library=shared -Dvulkan=disabled -Dvk-proc-addr=disabled -Dd3d11=disabled \
    -Dglslang=disabled -Dshaderc=disabled -Dlcms=disabled -Ddovi=enabled -Dlibdovi=disabled \
    -Ddemos=false -Dtests=false -Dopengl=enabled -Dgl-proc-addr=enabled
fi
ninja -C $W/build/libplacebo install

cmake -S $TP/cb1/CB1 -B $W/build/cb1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build $W/build/cb1
