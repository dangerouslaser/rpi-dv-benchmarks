#!/bin/bash
# FFmpeg 9.0 as LibreELEC builds it for PROJECT=RPi DEVICE=RPi5 (patch dirs
# postproc + libreelec + rpi; V4L2 m2m + request + SAND), plus CB1's Generic
# FFmpeg patches (a superset of yblod's three metadata patches, byte-identical).
# Runs inside the build container.
set -euo pipefail
TP=/work/third_party
SRC=/work/build/ffmpeg-src
LE=$TP/cb1/packages/multimedia/ffmpeg/patches
DV=$TP/cb1/projects/Generic/patches/ffmpeg

if [ ! -f "$SRC/.patched" ]; then
  rm -rf "$SRC"; mkdir -p "$SRC"
  tar -xJf $TP/ffmpeg-9.0.tar.xz -C "$SRC" --strip-components=1
  echo 9.0 >"$SRC/RELEASE"
  for p in $LE/postproc/*.patch $LE/libreelec/*.patch $LE/rpi/*.patch $DV/*.patch; do
    echo "patch $(basename "$p")"
    patch -d "$SRC" -p1 --forward --quiet <"$p"
  done
  touch "$SRC/.patched"
fi

mkdir -p /work/build/ffmpeg && cd /work/build/ffmpeg
if [ ! -f config.h ]; then
  "$SRC/configure" --prefix="$PREFIX" \
    --cpu=cortex-a76 --arch=aarch64 --target-os=linux \
    --enable-shared --disable-static --enable-pic \
    --enable-gpl --enable-version3 --disable-doc \
    --disable-debug --enable-optimizations --disable-extra-warnings \
    --enable-avcodec --enable-avformat --enable-swscale --enable-avfilter \
    --disable-devices --enable-pthreads --disable-network \
    --enable-v4l2_m2m --enable-libdrm --enable-libudev --enable-v4l2-request \
    --disable-mmal --enable-sand \
    --enable-vaapi --disable-vdpau \
    --enable-runtime-cpudetect --enable-neon --enable-hwaccels \
    --disable-encoders --enable-encoder=rawvideo --enable-encoder=wrapped_avframe \
    --disable-muxers --enable-muxer=rawvideo --enable-muxer=null --enable-muxer=framemd5 \
    --enable-demuxers --enable-parsers --enable-bsfs \
    --disable-bzlib --disable-lzma --disable-libdav1d
fi
make -j"$(nproc)"
make install
