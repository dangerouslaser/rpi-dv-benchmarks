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
| yblod Vulkan engine (libreelec-yblod 0.3.3 + `patches/yblod/`, 7 commits) | builds; `cargo test` 34/34; C CPU tests 4/4 |
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

## v3dv cost model and pre-hardware review (2026-11-06)

Raspberry Pi OS trixie ships **Mesa 25.0.7**, not Mesa main. Checked against
its v3dv source: same gaps as above plus `shaderInt16 = false` (enabled for
V3D 7.1 only later). Patch 0005 makes the Pi shader build need neither
shaderInt64 nor shaderInt16; without it the engine would not have started on
a stock Pi.

What the V3D compiler does (Mesa `v3dv_pipeline.c`, `nir_to_vir.c`):

- **All 64-bit integer maths is lowered to 32-bit** (`lower_int64_options = ~0`).
  A driver exposing shaderInt64 would generate about what our emulation
  writes by hand, so "emulated" isn't the cost; the arithmetic is.
- **Multipliers are 24-bit.** A 32-bit `imul` is 2 instructions (MULTOP +
  UMUL24); the high half of a 32x32 product (`umulExtended`/`imulExtended`)
  is lowered to 4 such multiplies plus ~10 adds/shifts/masks (Hacker's
  Delight). A 64-bit product therefore costs ~20+ instructions.
- **No integer divide.** Division by a non-constant is a long sequence;
  division by a non-power-of-two constant becomes a `mul_high` (expensive here).

Where that bites:

| Hot spot | Per | Cost driver | Possible change |
| --- | --- | --- | --- |
| FEL MMR chroma (`fel_chroma`, `fel_mmr_dual_horizontal`, `fel_mmr_single`) | chroma sample, both channels | up to 24 signed 32x32->64 products + 64-bit adds | features are < 2^20 (the route's eligibility proof): split into 24-bit-friendly partial products (`umul24` = 1 instruction) |
| `piecewise` (MMR single-layer) | pixel | 64x32 products | same |
| `rounded(v, d)` with a runtime `d` (`method ? 128 : 4`) | EL sample | integer divide | branch to two constant power-of-two divisions |
| `sand30_unpack` (ours) and the bench's GLES unpack for CB1 | sample | `n / 3`, `n % 3` (mul_high) | one invocation per 32-bit word (3 samples), no division |
| `pack.comp` resized path `px / dx`, CB1 overlay `e / 71` | pixel / tile | divide | only on resized / overlay frames |

CB1 does the same exact-integer MMR in GLES with `umulExtended` on `uvec2`
(libplacebo patch 9913), so on the Pi both engines pay this; the comparison
stays fair, but both could be faster.

Done before hardware (patch 0006): no integer division in `sand30_unpack`
(one invocation per word) or in `rounded()`'s runtime divisor (Pi build); the
bench's GLES unpack for CB1 divides by 3 in float. All byte-identical.

Not done, on purpose: hand-written 24-bit MMR products. Mesa has no rule
turning a plain multiply with small operands into `umul24`, and GLSL/SPIR-V
can't express `umul24`, so any decomposition still compiles to the same
2-instruction 32-bit multiplies; fewer products needs an algorithmic change,
to be chosen from the Pi's per-stage profile.

**Measure before changing:** v3dv has compute timestamps
(`timestampComputeAndGraphics`), so `run-bench.sh` now also runs the Vulkan
engine with `DVVK_PROFILE=1` (per-stage GPU ms: reconstruction, FEL stages,
pack). Optimise the stages that show up there.

## Day-one readiness (run-bench.sh)

- correctness first: CB1 == Vulkan on hardware-decoded pictures, and the
  Vulkan engine's output from hardware == software decoding of the same
  pictures (proves the SAND layout end to end; HEVC decoding is exact);
- performance: each engine in its own process, 3 repeats alternating which
  goes first, CPU governor set to performance, clocks / temperature /
  throttle flags recorded around every run, throttled runs excluded from the
  medians (`summarize.py` -> `table.txt`);
- real-time playback (`dvbench --play`): decoding thread with an 8-picture
  queue, rendering on the stream's frame clock, one frame of latency; late
  pictures and decode stalls counted;
- `DVVK_PROFILE=1` per-stage GPU times of the Vulkan engine.

`tests/check.sh --validate` also runs the Pi configuration under the Khronos
validation layer. Core validation is proven to fire (planted misaligned
fill); synchronization validation is enabled but did not report a planted
transfer->dispatch hazard on lavapipe with layers 1.4.309, so it is not
relied on.

## First Pi run (2026-11, Pi 500, RPi OS trixie, Mesa 26.2.2)

Hardware decode works; CB1 == Vulkan byte for byte on all three clips (12/12).
Decode 33-63 fps. Per picture: CB1 63-124 ms, Vulkan 297-737 ms (~5x slower).
Vulkan GPU time: pack 251-385 ms, reconstruction 40 ms (P5/MEL) / 344 ms
(FEL: chroma 174, luma_phase_guide 105, vertical_pair 11, phase_vertical_uv 6).

Fixes it needed, now in the repo: dmabuf import vs v3dv's 64-byte requirement
padding (patch 0007), non-exportable timeline semaphore (0007), the decoder's
32-buffer pool (bench caps hardware preloads at 12), CMA (needs
`dtoverlay=vc4-kms-v3d,cma-512`; run-bench warns), native builds on the Pi
(`scripts/install-native-deps.sh`, then `build-all.sh` runs without Docker).

Leading explanations for the gap, from Mesa's V3D compiler (to confirm with
the per-shader stats run-bench now collects, `*-shader-stats.txt`):

- **No on-chip shared memory**: `load_shared`/`store_shared` are ordinary TMU
  memory accesses on V3D. The engine's tiled passes (pack's EL/chroma/GUI
  tiles, fel_chroma's EL tile) stage data through shared memory with
  barriers, a win on desktop GPUs and pure extra traffic here.
- **Register pressure**: V3D runs 4/2/1 threads per QPU depending on a
  shader's registers, and only threads hide memory latency; big shaders
  (pack, fel_chroma with emulated 64-bit maths) may drop to 1 thread or
  spill. `V3D_DEBUG=shaderdb` shows threads and spills per pipeline.
- **Scalar 16-bit loads**: every `uint16_t` SSBO element is its own TMU
  access; CB1 reads the same planes as textures (TMU texture path, cached,
  4 samples per gather) and writes through the tile buffer.

CB1's own time: the bench now times its SAND->P010 GLES pass separately, and
the software-decode reference runs P010 and SAND input for both engines.

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
