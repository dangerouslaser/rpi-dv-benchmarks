#!/bin/bash
# out/dvbench-rpi5-<rev>.tar.gz: everything the Pi needs beyond Raspberry Pi
# OS (trixie, 64-bit): dvbench (the engine is linked in), vk-micro,
# probe-dmabuf, the matched FFmpeg, run-bench.sh. Build first (build-all.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=$ROOT/build/prefix
REV=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo -dirty)
NAME=dvbench-rpi5-$REV
STAGE=$ROOT/out/$NAME
rm -rf "$STAGE" && mkdir -p "$STAGE/bin" "$STAGE/lib"
cp "$P/bin/dvbench" "$P/bin/probe-dmabuf" "$P/bin/vk-micro" "$STAGE/bin/"
cp -P "$P"/lib/lib{avcodec,avformat,avutil,avfilter,swscale,swresample,postproc,avdevice}.so* "$STAGE/lib/" 2>/dev/null || true
cp "$ROOT/bundle/run-bench.sh" "$ROOT/bundle/summarize.py" "$ROOT/bundle/README.md" "$STAGE/"
{
  echo "bundle $NAME built $(date -u +%Y-%m-%dT%H:%MZ)"
  sed -n '/^[a-z]/p' "$ROOT/sources.lock"
  echo "ffmpeg patches:"; (cd "$ROOT/patches/ffmpeg" && ls */*.patch)
} >"$STAGE/VERSION"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT/out" -czf "$ROOT/out/$NAME.tar.gz" "$NAME"
echo "$ROOT/out/$NAME.tar.gz ($(du -h "$ROOT/out/$NAME.tar.gz" | cut -f1))"
