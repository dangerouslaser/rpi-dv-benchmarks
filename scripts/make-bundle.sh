#!/bin/bash
# out/dvbench-rpi5-<rev>.tar.gz: everything the Pi needs beyond Raspberry Pi
# OS (trixie, 64-bit): dvbench (--probe checks the machine), libpi_engine,
# the matched FFmpeg, run-bench.sh. Build first (build-all.sh).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
P=$ROOT/build/prefix
REV=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo -dirty)
NAME=dvbench-rpi5-$REV
STAGE=$ROOT/out/$NAME
rm -rf "$STAGE" && mkdir -p "$STAGE/bin" "$STAGE/lib"
cp "$P/bin/dvbench" "$STAGE/bin/"
ls "$P/lib/libpi_engine.so."* >/dev/null 2>&1 || { echo "missing $P/lib/libpi_engine.so.*: build the engine first" >&2; exit 1; }
cp -P "$P/lib/libpi_engine.so"* "$STAGE/lib/"
# FFmpeg's libraries: the ones dvbench links must be there; the rest of the
# build (avfilter, swscale, ...) goes along when it was built.
for lib in avcodec avformat avutil; do
  ls "$P/lib/lib$lib.so."* >/dev/null 2>&1 || { echo "missing $P/lib/lib$lib.so.*: build FFmpeg first (scripts/build-all.sh)" >&2; exit 1; }
done
for lib in avcodec avformat avutil avfilter swscale swresample postproc avdevice; do
  for f in "$P/lib/lib$lib.so"*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    cp -P "$f" "$STAGE/lib/"
  done
done
cp "$ROOT/bundle/run-bench.sh" "$ROOT/bundle/summarize.py" "$ROOT/bundle/README.md" "$STAGE/"
{
  echo "bundle $NAME built $(date -u +%Y-%m-%dT%H:%MZ)"
  sed -n '/^[a-z]/p' "$ROOT/sources.lock"
  echo "ffmpeg patches:"; (cd "$ROOT/patches/ffmpeg" && ls */*.patch)
} >"$STAGE/VERSION"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$ROOT/out" -czf "$ROOT/out/$NAME.tar.gz" "$NAME"
echo "$ROOT/out/$NAME.tar.gz ($(du -h "$ROOT/out/$NAME.tar.gz" | cut -f1))"
