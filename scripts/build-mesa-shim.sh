#!/bin/bash
# Mesa's V3D drivers (v3dv, v3d GL) and its V3D drm-shim, into build/mesa-v3d:
# with the shim preloaded the real V3D compiler runs without a Pi (it emulates
# the Pi 5's V3D 7.1; job submission does nothing), so V3D_DEBUG=shaderdb
# reports every shader's instructions, threads and spills offline
# (scripts/shader-stats.sh). Runs inside the build container (or natively).
#   build-mesa-shim.sh [mesa|mesa-main]   (the sources.lock entry; default mesa)
set -euo pipefail
W=${WORK:-/work}
NAME=${1:-mesa}
SRC=$W/third_party/$NAME
OUT=$W/build/$NAME-v3d
BLD=$W/build/$NAME-build
if [ ! -f "$BLD/build.ninja" ]; then
  meson setup "$BLD" "$SRC" --prefix="$OUT" --libdir=lib --buildtype=release \
    -Dvulkan-drivers=broadcom -Dgallium-drivers=v3d,vc4 -Dtools=drm-shim \
    -Dplatforms= -Dglx=disabled -Degl=enabled -Dgbm=disabled -Dgles1=disabled -Dgles2=enabled \
    -Dllvm=disabled -Dvideo-codecs= -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=enabled \
    -Dbuild-tests=false -Dshared-glapi=enabled -Dglvnd=disabled
fi
ninja -C "$BLD" install >/dev/null
ls "$OUT"/lib/libv3d_noop_drm_shim.so "$OUT"/share/vulkan/icd.d/broadcom_icd.*.json
