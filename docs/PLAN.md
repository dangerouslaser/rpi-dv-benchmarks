# Raspberry Pi 5: yblod Vulkan engine vs CB1 (GLES) benchmark

Goal: measure decode and frame generation speed of yblod's Vulkan engine and
CroqueMr's CB1 (libplacebo on GLES) on Pi 5 hardware. Not a LibreELEC/Kodi
image: a headless benchmark binary plus the two engines and a matched FFmpeg.

The harness is `dvvk-mr-compare` from intel-dv-libreelec (yblod-engine/vk/tools),
which already runs both engines on the same decoded frames: `--only cb1|vk`
per-picture timing and `--throughput S` pipelined fps + CPU time per frame,
plus byte-for-byte comparison of the two engines' output.

## Status (2026-11-06, no hardware yet)

| Piece | aarch64 build | Tests |
| --- | --- | --- |
| FFmpeg 9.0, LibreELEC RPi5 config (`rpi` patch, V4L2 request HEVC, SAND) + CB1/yblod DV patches | ok | all 14 patches apply cleanly |
| libplacebo e2972fd + CB1's 21 patches (GLES, LibreELEC options) | ok | CB1's ABI probes pass |
| CB1 `dvbridge_core` (CroqueMr RC3, 00615a5) | ok | not yet run |
| yblod Vulkan engine `libdv_engine_vk.so.1` (libreelec-yblod 0.3.3) | ok, 1 patch | `cargo test` 34/34 |
| yblod C host code CPU tests | ok | ctest 4/4 |
| Benchmark harness (`dvvk-mr-compare`) | not yet ported | |

`scripts/build-all.sh` reproduces all of it from `sources.lock`.

## What the Pi 5 driver (Mesa v3dv, V3D 7.1) lacks

Read from Mesa source (main, 27.0-devel, `src/broadcom/vulkan/v3dv_device.c`):

| Engine needs | v3dv | Port work |
| --- | --- | --- |
| `shaderInt64` (7 shaders + `dv_common.glsl`, ~40 sites: FEL MMR, piecewise reshape, table sums) | **no** | emulate 64-bit maths on 32-bit pairs (`umulExtended`/`imulExtended`); verify bit-exact against native int64 on lavapipe |
| `VK_KHR_push_descriptor` (every dispatch, `gpu.rs::PushDescriptor::bind`) | **no** | fallback: descriptor sets from a per-frame pool, same `bind()` signature |
| VAAPI surfaces (`interop/va.rs`, `tools/decode.c`) | no VAAPI on Pi | new input: FFmpeg DRM_PRIME frames from V4L2 request decode |
| Decoded frame layout | Pi decoder outputs **SAND128 column** NV12/P030 only; v3dv can't import SAND modifiers | import the dmabuf as a VkBuffer, de-column in the read shader (`interop_read_planes.comp`) |
| int16, 8/16-bit storage, timeline semaphores, sync2, maintenance4, subgroup size control, dma_buf, drm modifiers, external semaphore fd, queue_family_foreign | yes (int16/int8 only on V3D 7.1 = Pi 5) | none |
| `calibrated_timestamps` | no | optional already |
| Subgroups | 16 wide; basic/vote/ballot/shuffle/arith/quad/clustered on 7.1 | check `DVVK_SUBGROUP` defaults against 16 |
| Shared memory 32 KiB, 256 invocations/workgroup | | check shader limits |
| AMD compute power profile (`power.rs`) | n/a | no-op on Pi |

CB1 side: CB1 imports P010 planes as R16/GR1616 EGLImages. Mesa's v3d GL
driver imports SAND30 (P030 + SAND128 modifier) and converts it with an
internal blit (`v3d_blit.c`, `sand30_blit`), so CB1 needs a SAND-aware import
(whole-frame P030 import, or a de-SAND pass) rather than per-plane R16.
That conversion cost belongs in CB1's timing on the Pi.

## Work without hardware (in order)

1. **Push descriptor fallback.** Testable now on lavapipe by forcing it on.
2. **int64 emulation in shaders.** Oracle: run each shader native-int64 vs
   emulated on lavapipe over randomized inputs, require identical outputs;
   then the golden hashes (`tools/golden.sha256`).
3. **A `v3dv` mode on lavapipe** (`DVVK_PI5=1`): refuse int64 + push
   descriptors, subgroup 16, Pi limits; so the Pi's code paths run in CI.
4. **DRM_PRIME / SAND frame input** for the Vulkan engine (new `dvvk_frame`
   input kind next to VASurfaceID; ABI bump), and SAND-aware import for CB1.
   SAND30 address maths unit-tested on CPU; full path tested in a Debian arm64
   VM with udmabuf (Docker Desktop's and OrbStack's kernels have no udmabuf),
   feeding software-decoded frames re-laid out as SAND30 dmabufs.
5. **Port `dvvk-mr-compare`** to the Pi: V4L2 request decode (DRM_PRIME),
   surfaceless EGL on the v3d render node for CB1, Vulkan on v3dv; add stage
   timers (decode, import, GPU, total) and JSON output for both engines.
6. **Bundle**: one tarball (bench + engines + FFmpeg + libplacebo, rpath'd)
   that runs on Raspberry Pi OS trixie 64-bit; a `run-bench.sh` that probes
   (`vulkaninfo`, `v4l2-ctl`, Mesa version), runs every clip/profile for both
   engines and writes one report.

## Day one on the Pi

Raspberry Pi OS trixie 64-bit (or a LibreELEC RPi5 dev image), copy the
bundle, `run-bench.sh /path/to/clips`. Clips: the golden set (P5, P8.1,
letterboxed P8.1, P7 MEL, P7 FEL); 4K P7 FEL matters most (dual decode).

## Open questions

- Decode budget for P7 FEL: Pi 5 has one HEVC block; 4K BL + 1080p EL per
  frame, or EL in software on the A76 cores.
- Which Mesa the Pi image ships (RPi OS trixie vs LibreELEC) — v3dv features
  above are from Mesa main; recheck with `vulkaninfo` on the device.
- Whether CroqueMr has an RPi/GLES-specific CB1 path or expects the x86 one.
