#!/bin/bash
# libplacebo (pinned, CB1's patches, LibreELEC's options: OpenGL/GLES only)
# then CB1's dvbridge_core static library. Runs inside the build container.
set -euo pipefail
TP=/work/third_party
SRC=/work/build/libplacebo-src
if [ ! -f "$SRC/.patched" ]; then
  rm -rf "$SRC"; cp -a $TP/libplacebo "$SRC"; rm -rf "$SRC/.git"
  git -C "$SRC" init -q   # meson's version script wants a git tree or VERSION
  for p in $TP/cb1/CB1/patches/libplacebo/*.patch; do
    echo "patch $(basename "$p")"
    patch -d "$SRC" -p1 --forward --quiet <"$p"
  done
  touch "$SRC/.patched"
fi
if [ ! -f /work/build/libplacebo/build.ninja ]; then
  meson setup /work/build/libplacebo "$SRC" --prefix="$PREFIX" --libdir=lib --buildtype=release \
    -Ddefault_library=shared -Dvulkan=disabled -Dvk-proc-addr=disabled -Dd3d11=disabled \
    -Dglslang=disabled -Dshaderc=disabled -Dlcms=disabled -Ddovi=enabled -Dlibdovi=disabled \
    -Ddemos=false -Dtests=false -Dopengl=enabled -Dgl-proc-addr=enabled
fi
ninja -C /work/build/libplacebo install

cmake -S $TP/cb1/CB1 -B /work/build/cb1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build /work/build/cb1
