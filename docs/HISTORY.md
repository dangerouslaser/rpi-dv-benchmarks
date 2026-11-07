# How we got here

## 1. Benchmarking the existing engines (before and on day one)

The first milestone was to compile yblod's Vulkan engine for ARM and
benchmark it on the Pi 5 against CroqueMr's CB1 (libplacebo on GLES).

Before the hardware arrived:
- **Port.** yblod's engine was ported to the Pi: no `shaderInt64` or
  `shaderInt16`, no push descriptors, no VAAPI, SAND128 input, bilinear EL.
- **Verification.** Its output was checked identical to CB1 Master
  Reference's in a Debian arm64 VM on lavapipe.
- **Bench.** A shared bench (`dvbench`) decoded with the Pi's V4L2 decoder.

On the first Pi run, yblod's engine took 297–737 ms per picture and CB1 63–124
ms. Two rounds of tuning yblod's compute shaders (specialisation, narrow
variants, 4-thread register budgets) moved it only 1–26%.

## 2. Understanding V3D

`docs/pi5/V3DV.md` collects the Pi 5 GPU and Mesa's v3dv driver, from source,
from the device and from Igalia's material. The decisive facts:
- **Every compute dispatch is its own kernel job,** with a cache invalidate
  and a synchronous clean.
- **Fragment output stays in the on-chip tile buffer.**
- **The ALUs are scalar:** no FMA, 32-bit integer multiplies take two
  instructions, and there is no 64-bit arithmetic.

`vk-micro` then measured the same 4K shader every way:
- Vulkan and GLES **fragment** passes ran at the same speed.
- **Compute** was about 10× slower in both APIs.

The engine was slow because of its desktop-style design, not because it was
Vulkan.

## 3. The Pi engine

The redesign, in steps:
1. **First version:** fragment passes, the decoder's buffers read directly,
   and yblod's planner on the CPU. It was bit-exact against yblod (bilinear
   FEL) from the start. FEL clips took 121–161 ms per picture.
2. **Specialisation and fewer fetches:** 72–88 ms.
3. **4:2:2 and pack fused, no local arrays:** 1917 at 44.8 ms.
4. **Letterbox skipping:** 35.3 ms float.
5. **Exact MMR at near float cost** (split and bounded) and **no rolled
   loops:** 1917 plays exactly in real time at 33–35 ms.
6. **The single-layer routes:** Dune (MEL) at 16 ms, and profile 8.1 MMR
   at 37 ms.

Tested on the NAS's real films over SMB: 1917 and Dune.

## 4. Cleanup

Once the engine stood on its own, the yblod and CB1 reference engines left
the repository:
- **The planner** (yblod's C code that turns the RPU into tables) is vendored
  in `engine/planner/`.
- **FFmpeg's LibreELEC and Dolby Vision patches** are vendored in
  `patches/ffmpeg/`.
- **The equivalence with yblod,** verified one last time on every test clip,
  lives on as `tests/golden/hashes.txt`.

The reference engines are still public:
- yblod: [dangerouslaser/libreelec-yblod](https://github.com/dangerouslaser/libreelec-yblod)
  at `00d9aa1a72`
- CB1: [CroqueMr/libreelec-x86-DV](https://github.com/CroqueMr/libreelec-x86-DV)
  at `00615a5a5e`

This repository's own history has the port's patches and every intermediate
measurement.
