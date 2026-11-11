# LibreELEC patches

Against LibreELEC.tv `a0fa50bc6e` with Vulkan on for the RPi5
(`VULKAN="vulkan-loader"` in `projects/RPi/devices/RPi5/options`) and the
kernel series `../linux/` in `projects/RPi/patches/linux/`. Apply in order
with `git am`.

| Patch | What |
|---|---|
| `0001` pi-dv-engine: initial package | `packages/multimedia/pi-dv-engine/package.mk`: `libpi_engine.so.1`, `pi_engine.h`, `pi_engine.pc` (CMake, `-DPI_BUILD_BENCH=OFF -DPI_SHARED=ON`). Needs ffmpeg (libavutil), vulkan-headers and vulkan-loader. `PKG_VERSION` / `PKG_SHA256` follow the release tarball `scripts/make-release.sh` makes. |
| `0002` ffmpeg: rpi: leave the decode queue when no dst buffer is free | `packages/multimedia/ffmpeg/patches/rpi/0002-…`: in the V4L2 request HEVC decoder (`libavcodec/v4l2_req_hevc_vx.c`), `v4l2_request_hevc_start_frame()` queued the frame before allocating its destination buffer and left it queued when that failed (CMA exhausted), so closing the decoder hung in `avcodec_free_context`. |
| `0003` kodi: optional Dolby Vision output (DOLBY_VISION_SUPPORT) | `packages/mediacenter/kodi/package.mk`: with `DOLBY_VISION_SUPPORT=yes`, depends on pi-dv-engine and builds with `-DENABLE_PIDV=ON` (else `OFF`), as for VAAPI. |
| `0004` RPi5: Dolby Vision output | `DOLBY_VISION_SUPPORT="yes"` in `projects/RPi/devices/RPi5/options`, and `../kodi/` as `projects/RPi/devices/RPi5/patches/kodi/`. |
| `0005` pi-dv-engine: download from the libreelec-rpi-dv release | `PKG_URL` / `PKG_SITE`: the engine tarball is an asset of the [libreelec-rpi-dv](https://github.com/dangerouslaser/libreelec-rpi-dv) release `v<version>`. |
| `0006` pi-dv-engine: update to 0.2.0 | `PKG_VERSION` / `PKG_SHA256` of the 0.2.0 tarball. |
| `0007` pi-dv-engine: update to 0.3.0 | `PKG_VERSION` / `PKG_SHA256` of the 0.3.0 tarball: `libpi_engine.so.2`, the overlay input. |
| `0008` RPi5: kodi: composite subtitles into the Dolby Vision signal | `../kodi/0007` as `projects/RPi/devices/RPi5/patches/kodi/0007-…`. |
| `0009` ffmpeg: rpi: H.264 MVC multiview decoding | `packages/multimedia/ffmpeg/patches/rpi/0003-…`: Dom Cobley's H.264 MVC series ([FFmpeg pull request 24303](https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls/24303)), with the master h264 changes it builds on, backported to 9.0.2; FATE tests left out. The h264 decoder outputs the dependent view when asked to (`view_ids`). |
| `0010` RPi5: kodi: stereoscopic 3D playback | `projects/RPi/devices/RPi5/patches/kodi/0008–0046`: Dom Cobley's stereoscopic 3D series ([popcornmix/xbmc `fix-stereoscopic-3d-gbm-upstream`](https://github.com/popcornmix/xbmc/tree/fix-stereoscopic-3d-gbm-upstream)) on `../kodi/0001–0007`: HDMI frame-packed and half / full side-by-side and top-and-bottom modes with an eye per video plane, H.264 MVC and MV-HEVC to two views, Blu-ray 3D, 3D subtitles at the disc's depth. Each patch keeps its author; where one met the Dolby Vision patches, its message says how. |
| `0011` RPi5: kodi: the Dolby Vision options as settings in Player / Videos | `../kodi/0008` as `projects/RPi/devices/RPi5/patches/kodi/0047-…`. |
| `0012` RPi5: LibreELEC-settings: update from this repository's releases | `projects/RPi/devices/RPi5/patches/LibreELEC-settings/0001-…`: the update menu reads its channels from `releases.json` on the LibreELEC tree's `main` (DV-13.0: the releases, DVbeta-13.0: the pre-releases; `scripts/releases-json.py` writes it), and the update check offers the newest build of the running build's channel instead of asking LibreELEC's update server. Builds are named `…-13.0-dv-<version>` (`CUSTOM_VERSION=dv-<version>` for `make image`), which its `prettyname_regex` reads. |
| `0013` pi-dv-engine: update to 0.4.0 | `PKG_VERSION` / `PKG_SHA256` of the 0.4.0 tarball (`libpi_engine.so.3`, overlay areas), which now downloads from the release `engine-<version>`. |
| `0014` RPi5: kodi: composite the GUI into the Dolby Vision signal | `../kodi/0009` as `projects/RPi/devices/RPi5/patches/kodi/0048-…`. |
| `0015` pi-dv-engine: update to 0.4.1 | `PKG_VERSION` / `PKG_SHA256` of the 0.4.1 tarball: faster, the same output and API (`libpi_engine.so.3`). |

**Building:** `make image` as usual (`PROJECT=RPi DEVICE=RPi5 ARCH=aarch64`).
The engine tarball downloads from the release. To build an engine version
before its release exists, put the tarball `scripts/make-release.sh` makes
where `get` finds it: `sources/pi-dv-engine/pi-dv-engine-<version>.tar.xz`
with its `.sha256` (and a `.url`), and its SHA-256 in `package.mk`.
