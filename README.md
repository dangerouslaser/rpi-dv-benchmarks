<img src="assets/icon.svg" width="64" height="64" alt="">

# librelec-rpi5-dv

Dolby Vision for the Raspberry Pi 5, aimed at LibreELEC 13.

A Vulkan engine reconstructs Profile 7 FEL / MEL, 8.1 and 8.1 MMR streams
from the Pi's own HEVC decoder. It then packs them, with their metadata, into
the TV-led HDMI tunnel frame. The output is bit-exact against yblod's Vulkan
engine (bilinear FEL) and fast enough for real-time 24p 4K FEL on a stock Pi 5.

| Film, Pi 5 at stock clocks | Per picture | Real time at 23.976 fps |
|---|---|---|
| 1917: UHD Blu-ray, Profile 7.6 FEL, MMR, exact | 35.2 ms | 5 minutes, 0 late frames |
| Dune (2021): Profile 7.6 MEL | 16.1 ms | 60 s, 0 late |
| Profile 8.1 with MMR chroma | 37 ms (27 fps flat out) | close: 8 late in an 8 s test, by ≤ 4 ms |

The design, its routes, MMR modes and measurements are in
[docs/ENGINE.md](docs/ENGINE.md). The Pi 5 GPU and its Vulkan driver are in
[docs/pi5/V3DV.md](docs/pi5/V3DV.md). How we got here is in
[docs/HISTORY.md](docs/HISTORY.md). The open review findings are in
[docs/REVIEW.md](docs/REVIEW.md).

## Layout

| Path | What |
|---|---|
| `engine/` | The engine: `pi_engine.c`, its shaders (`shaders/`: two fragment passes), the planner (`planner/`: the CPU half, vendored from yblod, GPL-3.0-or-later) |
| `bench/` | `dvbench`: decode (the Pi's V4L2 decoder, or software into the same SAND128 layout) + the engine, timed, with playback and output hashes; `vk-micro`: compute vs fragment, Vulkan vs GLES; `probe-dmabuf` |
| `patches/ffmpeg/` | FFmpeg 9.0 as LibreELEC builds it for the Pi 5, plus the Dolby Vision metadata patches (`patches/ffmpeg/README.md`) |
| `tests/` | `check.sh`: the engine must reproduce `golden/hashes.txt` (verified identical to yblod); `make-test-clips.sh`: profile 8.1 / MEL / MMR clips from Profile 7 sources |
| `bundle/` | What goes onto the Pi: `run-bench.sh` (correctness, speed, MMR modes, real-time playback, shader statistics), `summarize.py` |
| `image/`, `scripts/make-image.sh` | A flashable Raspberry Pi OS image with the bundle and clips |
| `vm/`, `scripts/vm*.sh` | A Debian arm64 VM (Lima) on Mesa's software Vulkan, for building and checking without a Pi |
| `docker/`, `scripts/build-*.sh` | The aarch64 build container and builds |
| `docs/` | `ENGINE.md`, `pi5/V3DV.md` (+ `pi5/device/`: the Pi's own Vulkan / system dumps), `SETUP.md`, `HISTORY.md`, `REVIEW.md` (open review findings) |

## Quick start (Apple Silicon Mac: Docker and `brew install lima`)

```sh
./scripts/build-all.sh               # aarch64 in Docker: FFmpeg (+ patches), the engine, the tools, the Pi bundle
./scripts/vm-setup.sh                # the test VM (vm/rpi5dv.yaml)
scripts/vm.sh tests/check.sh         # the engine against the golden hashes (needs clips/)
scripts/vm.sh dvbench --hash clips/X.mkv
```

On the Pi: flash `out/dvbench-rpi5-sd.img.xz` (`scripts/make-image.sh`) or
copy `out/dvbench-rpi5-*.tar.gz` to an existing install, then run
`./run-bench.sh clips/*`. See [bundle/README.md](bundle/README.md).

More in [docs/SETUP.md](docs/SETUP.md). `sources.lock` pins FFmpeg;
`scripts/fetch-sources.sh` fetches it into `third_party/`. Media lives in
`clips/`, which isn't in git.

## Status

- **Done:** the engine and its routes, exactness, and real-time performance
  on hardware (above).
- **Next:** LibreELEC 13 integration: Vulkan in our LE branch, the output on
  a DRM PRIME plane, DV signalling on the Pi's HDMI, Kodi. Details in
  [docs/ENGINE.md](docs/ENGINE.md) "Open".
