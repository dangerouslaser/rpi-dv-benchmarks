#!/bin/bash
# out/dvbench-rpi5-<rev>.tar.gz: everything the Pi needs beyond Raspberry Pi
# OS (trixie, 64-bit): dvbench, both engines, the matched FFmpeg/libplacebo,
# libva (the engine links it), run-bench.sh. Build first (build-all.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=$ROOT/build/prefix
REV=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo -dirty)
NAME=dvbench-rpi5-$REV
STAGE=$ROOT/out/$NAME
rm -rf "$STAGE" && mkdir -p "$STAGE/bin" "$STAGE/lib"
cp "$P/bin/dvbench" "$P/bin/probe-dmabuf" "$P/bin/vk-micro" "$P/bin/dvvk-int64-check" "$STAGE/bin/"
cp -P "$P"/lib/lib{avcodec,avformat,avutil,avfilter,swscale,swresample,postproc,avdevice}.so* "$STAGE/lib/" 2>/dev/null || true
cp -P "$P"/lib/libplacebo.so* "$P/lib/libdv_engine_vk.so.1" "$STAGE/lib/"
"$ROOT/scripts/in-docker.sh" bash -c 'cp -L /usr/lib/aarch64-linux-gnu/libva.so.2 /usr/lib/aarch64-linux-gnu/libva-drm.so.2 "${WORK:-/work}/out/'"$NAME"'/lib/"'
cp "$ROOT/bundle/run-bench.sh" "$ROOT/bundle/summarize.py" "$ROOT/bundle/shader-stats.py" "$ROOT/bundle/README.md" "$STAGE/"
{
  echo "bundle $NAME built $(date -u +%Y-%m-%dT%H:%MZ)"
  sed -n '/^[a-z]/p' "$ROOT/sources.lock"
  echo "engine patches:"; ls "$ROOT/patches/yblod"
} >"$STAGE/VERSION"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT/out" -czf "$ROOT/out/$NAME.tar.gz" "$NAME"
echo "$ROOT/out/$NAME.tar.gz ($(du -h "$ROOT/out/$NAME.tar.gz" | cut -f1))"
