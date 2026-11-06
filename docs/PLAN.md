# Raspberry Pi 5: yblod Vulkan engine vs CB1 (GLES) benchmark

Goal: measure decode and frame generation speed of yblod's Vulkan engine and
CroqueMr's CB1 (libplacebo on GLES) on Pi 5 hardware. Not a LibreELEC/Kodi
image: a headless benchmark (`dvbench`), both engines and a matched FFmpeg,
shipped as one tarball with `run-bench.sh`.

## Status (2026-11-06, before hardware)

Everything below runs on aarch64 today; only the Pi's own hardware paths
(V4L2 request decoding, v3dv, v3d GLES) wait for the board.

| Piece | State |
| --- | --- |
| FFmpeg 9.0, LibreELEC RPi5 config (`rpi` patch, V4L2 request HEVC, SAND) + CB1's DV patches | builds; all 14 patches apply |
| libplacebo e2972fd + CB1's 21 patches; CB1 `dvbridge_core` (CroqueMr RC3 00615a5) | builds; ABI probes pass |
| yblod Vulkan engine (libreelec-yblod 0.3.3 + `patches/yblod/`, 4 commits) | builds; `cargo test` 34/34; C CPU tests 4/4 |
| `dvbench` (port of `dvvk-mr-compare` to DRM PRIME frames) | runs both engines in a Debian arm64 VM (lavapipe / llvmpipe) |
| Pi bundle `out/dvbench-rpi5-*.tar.gz` + `run-bench.sh` | self-contained; smoke-tested in the VM |

`./scripts/build-all.sh` reproduces all of it from `sources.lock`.

## Engine changes for the Pi 5 (`patches/yblod/`)

From Mesa's v3dv source (main): the Pi 5's driver lacks things the engine used.

| Pi 5 gap | Change | Verified (lavapipe, byte-identical output) |
| --- | --- | --- |
| no `shaderInt64` | every shader also built as `@noi64`: 64-bit maths on `uvec2` words (`dv_common.glsl`); picked when the device lacks int64, `DVVK_NO_INT64=1` forces | native SPIR-V byte-identical to before; `dvvk-int64-check`: 12 helpers x 4.2M inputs vs native int64 (a planted carry bug is caught); engine output native == emulated on P7 FEL (MMR 64-bit LUT + polynomial chroma), P8.1 (route 2) and MMR single-layer (route 3, piecewise) |
| no `VK_KHR_push_descriptor` | descriptor sets from per-command-buffer pools; `DVVK_NO_PUSH_DESCRIPTOR=1` forces | identical to push descriptors on every clip |
| no VAAPI | `dvvk_set_surface_dmabuf`: callers register decoded DRM PRIME pictures by id; `va_display` may be NULL | |
| decoder outputs SAND128 P030 only; v3dv can't import SAND | SAND buffers imported as `VkBuffer`s, unpacked each frame (`sand30_unpack.comp`) into P010-identical planes; one or two objects | SAND input == linear P010 input |
| (tests) no DRM node / no `external_semaphore_fd` on lavapipe | `drm_fd < 0` picks a device by name; outputs without implicit fence (caller waits) | |
| aarch64 `c_char` is `u8` | build fix | |

Checked statically against v3dv: workgroups <= 256 invocations (limit 256),
shared memory <= ~17 KiB (limit 32 KiB), no subgroup operations in shaders,
every other extension/feature present (int16/int8 only on V3D 7.1 = Pi 5).

## The benchmark (`bench/`)

`dvbench` decodes N pictures first (decode speed), then per engine:
per-picture import + render + GPU finish (median/p95), optional flat-out
pipelined throughput (fps, CPU ms/frame), optional output hashes.

- Sources: `--source v4l2` (the Pi's HEVC decoder, SAND128) or `--source sw`
  (software decode copied into dmabufs as the Pi's exact SAND128 layout or
  linear P010). Buffers: udmabuf, else DMA heap (`linux,cma`, `system`).
- CB1 has no SAND input; the bench unpacks SAND into R16/RG16 textures with a
  GLES pass (one EGL image per 128-byte column) and counts it in CB1's time,
  mirroring the Vulkan engine's compute unpack. How CB1 should take SAND in
  Kodi on the Pi is CroqueMr's call.
- Cross-check: CB1 Master Reference and the Vulkan engine (CB1 contract) give
  byte-identical output in the VM on every picture tried, linear and SAND.

## Day one on the Pi

Raspberry Pi OS trixie 64-bit, `bundle/README.md`:

```sh
sudo apt install mesa-vulkan-drivers libvulkan1 libegl1 libgles2 vulkan-tools mesa-utils-bin v4l-utils
./run-bench.sh /path/to/clips/*
```

It records the system (Mesa, v3dv features, decoders, clocks, throttling),
runs each clip from the hardware decoder (retrying with the EL in software if
both layers can't share the decoder), cross-checks CB1 vs Vulkan output on the
Pi's GPUs, and repeats from software decode for reference.

## Untested until the board arrives

- FFmpeg's V4L2 request path end to end: DRM PRIME output, SAND layout as
  described by `v4l2_req_hevc_vx.c`, two decoder contexts for P7 BL + EL.
- v3dv importing the decoder's (CMA) dmabufs as buffers; v3d GLES importing
  128-byte-pitch RGBA8 column images and rendering R16/RG16.
- Real numbers: everything measured so far is CPU-emulated GPUs.
- DMA-heap allocation (the VM has udmabuf only).

## Open questions

- Decode budget for P7 FEL: one HEVC block, 4K BL + 1080p EL per frame,
  or the EL in software on the A76 cores (`--el-sw`).
- Which Mesa the target image ships (RPi OS trixie vs LibreELEC).
- CB1 on the Pi: does CroqueMr want SAND import inside CB1, or a shared
  unpack in Kodi's DRM PRIME path?
- Golden clips (`tools/golden.sha256`: 1917 FEL, Gemini MEL, 28YL P8.1) for
  the N150 oracle; the VM checks used local FEL test clips and P8.1/MMR
  clips made from them with dovi_tool.
