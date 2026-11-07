# FFmpeg patches

Applied in this order to FFmpeg 9.0 (`sources.lock`) by
`scripts/build-ffmpeg.sh`.

| Directory | From | What |
|---|---|---|
| `postproc/`, `libreelec/`, `rpi/` | LibreELEC's `packages/multimedia/ffmpeg/patches/`, as LibreELEC builds FFmpeg for `PROJECT=RPi DEVICE=RPi5` | The Raspberry Pi V4L2 request (stateless) HEVC decoder, SAND128 output and DRM PRIME frames |
| `dolby-vision/` | CroqueMr's [libreelec-x86-DV](https://github.com/CroqueMr/libreelec-x86-DV) (`projects/Generic/patches/ffmpeg`), at `00615a5a5e` | The Dolby Vision metadata the planner reads: raw RPU fields (`DV_FFMPEG_RAW_EXTENSIONS`), the dual-layer association in Matroska / MP4 / TS, and the EL |

The LibreELEC sets were taken from that same tree, which carries LibreELEC.tv's
packages. LibreELEC 13 master ships FFmpeg 9.0.2 with the same `rpi` set;
re-take them from LibreELEC.tv when moving the pin.

Licence: FFmpeg's (LGPL-2.1-or-later, GPL with `--enable-gpl`, as built here).
