#!/bin/bash
# FFmpeg 9.0 as LibreELEC builds it for PROJECT=RPi DEVICE=RPi5 (the postproc,
# libreelec and rpi patch sets: V4L2 request decoding of the Pi's HEVC into
# SAND128), plus the Dolby Vision metadata patches the planner reads
# (patches/ffmpeg/README.md). Runs inside the build container.
# Rebuilds from a fresh tree whenever the tarball, the patches (names, order or
# contents) or the configure flags change (a stamp of all three).
set -euo pipefail
W=${WORK:-/work}   # this repo: /work in the build container, the checkout natively
SRC=$W/build/ffmpeg-src
BUILD=$W/build/ffmpeg
PATCHES=$W/patches/ffmpeg
TARBALL=$W/third_party/ffmpeg-9.0.tar.xz

FLAGS=(--prefix="$PREFIX"
  --cpu=cortex-a76 --arch=aarch64 --target-os=linux
  --enable-shared --disable-static --enable-pic
  --enable-gpl --enable-version3 --disable-doc
  --disable-debug --enable-optimizations --disable-extra-warnings
  --enable-avcodec --enable-avformat --enable-swscale --enable-avfilter
  --disable-devices --enable-pthreads --disable-network
  --enable-v4l2_m2m --enable-libdrm --enable-libudev --enable-v4l2-request
  --disable-mmal --enable-sand
  --disable-vaapi --disable-vdpau --disable-xlib --disable-libxcb
  --enable-runtime-cpudetect --enable-neon --enable-hwaccels
  --disable-encoders --enable-encoder=rawvideo --enable-encoder=wrapped_avframe
  --disable-muxers --enable-muxer=rawvideo --enable-muxer=null --enable-muxer=framemd5
  --enable-demuxers --enable-parsers --enable-bsfs
  --disable-bzlib --disable-lzma --disable-libdav1d)
PATCH_LIST=("$PATCHES"/postproc/*.patch "$PATCHES"/libreelec/*.patch "$PATCHES"/rpi/*.patch "$PATCHES"/dolby-vision/*.patch)

sha() { if command -v sha256sum >/dev/null; then sha256sum; else shasum -a 256; fi | cut -d' ' -f1; }
# The source tree's stamp: the tarball and every patch, in application order.
SRC_STAMP=$({ basename "$TARBALL"; for p in "${PATCH_LIST[@]}"; do echo "== ${p#"$PATCHES"/}"; cat "$p"; done; } | sha)
# The build's stamp: the source tree's, plus the configure flags.
BUILD_STAMP=$({ echo "$SRC_STAMP"; printf '%s\n' "${FLAGS[@]}"; } | sha)

if [ "$(cat "$SRC/.patched" 2>/dev/null)" != "$SRC_STAMP" ]; then
  rm -rf "$SRC" "$BUILD"; mkdir -p "$SRC"
  tar -xJf "$TARBALL" -C "$SRC" --strip-components=1
  echo 9.0 >"$SRC/RELEASE"
  for p in "${PATCH_LIST[@]}"; do
    echo "patch ${p#"$PATCHES"/}"
    patch -d "$SRC" -p1 --forward --quiet <"$p"
  done
  echo "$SRC_STAMP" >"$SRC/.patched"
fi

# A fresh tree has old (tarball) mtimes, and new flags need a clean configure:
# either way start the build directory over.
if [ "$(cat "$BUILD/.stamp" 2>/dev/null)" != "$BUILD_STAMP" ]; then
  rm -rf "$BUILD"
fi
mkdir -p "$BUILD" && cd "$BUILD"
if [ ! -f config.h ] || [ ! -f .stamp ]; then
  "$SRC/configure" "${FLAGS[@]}"
  echo "$BUILD_STAMP" >.stamp
fi
make -j"$(nproc)"
make install
