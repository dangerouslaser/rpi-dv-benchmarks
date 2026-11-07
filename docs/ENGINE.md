# The engine

Dolby Vision for the Raspberry Pi 5, as two Vulkan fragment passes on the V3D
GPU (`engine/`).

It is TV-led, as in Kodi's DV engines for other platforms:
1. It reconstructs the composer's 12-bit signal from the base layer (BL), the
   enhancement layer (EL) and the RPU.
2. It packs that signal, with the display-management metadata, into the HDMI
   "tunnel" frame: 3840×2160 RGB 8-bit carrying 12-bit 4:2:2.
3. The TV does the display mapping. There is no tone mapping on the Pi.

## Pipeline

```
HEVC decoder (V4L2 request) ── BL, EL: SAND128 P030 dmabufs ──┐
RPU (AVDOVIMetadata) ── planner (CPU, ~0.5 ms) ── tables, packets ─┤
                                                                  ▼
P1  1920×1080   chroma: BL chroma phase, luma guide, EL chroma upsample,
                MMR / polynomial / piecewise chroma mapping + NLQ residual
                → RG16UI chroma (12-bit), R16UI EL luma
P2  1920×1080   one fragment per 2×2 output block: 4:2:2 vertical filter,
                luma (mapping + bilinear EL + NLQ), metadata bits, tunnel
                packing → the output dmabuf itself (two RG32UI images: its
                even and its odd rows)
```

- **One render job per pass.** Each pass is a single render pass, which v3dv
  runs as one V3D render job. There are no compute dispatches; v3dv makes
  every dispatch its own kernel job with a cache flush (`pi5/V3DV.md` §2).
- **Inputs are the decoder's own buffers.** They are read as `R32UI` texel
  buffers, with no detile copy.
- **The output is the scanout buffer.** P2 renders straight into the linear
  dmabuf. It needs one row of slack: `stride × (height + 1)` bytes.
- **Letterbox rows cost almost nothing.** P1 renders only the planner's row
  window. P2 clears the bars in the tile buffer and draws only their
  metadata-packet rows.

## Routes

The CPU half, the planner in `engine/planner/` (vendored from yblod), picks a
route per frame. The passes are specialised per stream: the route, chroma
phase method, MMR kinds and orders, and the 4:2:2 filter. Each pipeline is
built the first time a stream needs it.

| Route | Streams | P1 chroma | Luma |
|---|---|---|---|
| dual | Profile 7 FEL | MMR (or polynomial) + EL residual | mapping table + bilinear EL + NLQ |
| single | Profile 7 MEL, 8.1 with polynomial chroma | 12-bit tables | 12-bit table |
| piecewise | Profile 8.1 with MMR chroma (`--filters yblod`) | one MMR piece per channel: the exact split MMR; otherwise yblod's general reshape (exact, slow) | 12-bit table |

**Spatial filters** (`--filters`):
- **`cb1`, the default:** CB1 Master Reference's contract. Reshaping at 4:2:0,
  no chroma phase filter, Annex B 4:2:2 filters.
- **`yblod`:** YBLOD's chroma phase filters, the 2-tap 4:2:2 blend, and the
  full-height 4:2:2 layout for single-layer MMR. yblod's planner refuses
  single-layer MMR under CB1's contract.

**The FEL EL upscale is bilinear.** Annex B is the Master Reference filter;
it is not implemented in the passes.

## Exactness

The output is bit-exact against yblod's Vulkan engine with the bilinear EL
upscale: every sample of the tunnel frame, metadata included.
- **How it was proven:** sample by sample, on every clip and film point
  tested, before yblod was removed from this repository.
- **What keeps it:** `tests/golden/hashes.txt` carries those results.
  `tests/check.sh` must reproduce them.

## Chroma MMR modes (`--mmr`)

MMR is a 3-variable polynomial of up to 3rd order. Computed exactly it needs
64-bit integer arithmetic, which V3D doesn't have.

| Mode | How | Exact | 1917 |
|---|---|---|---|
| `exact`, the default | Float, with a per-sample error bound (see below). Samples whose bound straddles a 12-bit step take `split`. | yes | 35.2 ms |
| `split` | The 64-bit sum's low 32 bits from wrapping 32-bit integer multiply-adds, its high 32 bits from a float sum: `H = round(y − L / 2³²)`. The CPU checks each frame that float's worst-case error is under 0.25 (1917: 0.02); otherwise the frame uses `exact64`. | yes | 33.1 ms |
| `exact64` | yblod's emulated 64-bit (`umulExtended`) | yes | 45.8 ms |
| `exact32` | 16 + 16-bit × 10 + 10-bit pieces, four accumulators | yes | slower than `exact64` |
| `float` | 32-bit float | within 2 codes, about 0.001% of samples | 28.9 ms |

**The `exact` bound.** Vulkan's add and multiply are correctly rounded, so
the float value lies within 20 × 2⁻²⁴ × Σ|terms| of the exact one, with the
feature powers computed exactly. About 4.6% of 1917's chroma samples fall
inside that bound of a 12-bit step and take the exact path.

## Performance

Raspberry Pi 5, stock 1150 MHz GPU, no cooler, Mesa 26.2.6, hardware decode,
read over SMB:

| Film | Route | Per picture | 24p real time |
|---|---|---|---|
| 1917 (UHD BD, P7.6 FEL, MMR, 2.39:1) | dual, `exact` | 35.2 ms (29.0 fps) | 5 min, 7,192 frames, **0 late**, 69 °C, no throttling |
| Dune (2021) (UHD BD, P7.6 MEL) | single | 16.1 ms (65 fps) | 60 s, 0 late |
| Profile 8.1 MMR cuts (1917, test clips) | piecewise | 37.0–37.1 ms | 8 s, 8 late (≤ 4 ms) |
| DV FEL test clips (polynomial chroma) | dual | 29.0 ms (35 fps) | — |

Notes:
- The HEVC decoder alone does 44–79 fps for BL and EL, over SMB.
- The planner takes 0.4–0.7 ms of CPU per frame.
- For scale: yblod's compute engine takes 515 ms per picture on this GPU,
  CB1 Master Reference 111 ms.

**Overclocking.** With `v3d_freq=1300` the engine is about 10% faster and
still exact, but this test Pi has no cooler. The figures above are at stock
clocks; don't rely on an overclock.

## What V3D taught us (rules for these shaders)

1. **Fragment passes, never compute.** The same 4K shader is about 10× faster
   as a fragment pass than as compute, in Vulkan and GLES alike (`vk-micro`;
   `pi5/V3DV.md`). Vulkan and GLES fragment passes run at the same speed.
2. **Few passes, no intermediates.** The 4:2:2 filter and the pack, fused
   into one pass with the output written as two row-interleaved images, went
   from 42 to 17 ms.
3. **No local arrays filled in loops.** V3D's compiler places them in scratch
   memory before it unrolls the loop, so every value makes a TMU store and
   load round trip. Shader statistics don't report this as spills; the QPU
   dump shows `tmud`/`tmua` writes. Removing them took the fused pass from 31.8
   to 17.0 ms.
4. **One return per function, no loops.** Returns inside branches become
   loops, and in a large shader the compiler leaves them rolled. Removing
   them took the exact MMR from 37.6 to 33.1 ms (split) and from 44.1 to
   35.2 ms (bounded).
5. **The specialisation map must hold every constant.** An overflowed map
   silently built every MMR mode as the same pipeline.
6. **Fetch-bound and ALU-bound passes behave differently.** Below the fetch
   limit, time ≈ pixels × executed instructions × 1.6 × 10⁻¹¹ s at 1150 MHz.
   An empty 4K pass costs about 4 ms.
7. **Measured and dropped:**
   - tiled output plus a copy (+17 ms)
   - double-buffered tiles (no change)
   - `textureGather` for the 4:2:2 rows (slower)
   - 2×4 blocks per fragment (slower: 32 bytes a pixel of tile buffer
     shrinks the tiles)
   - texel-buffer views at word offsets (v3dv's offset alignment is
     64 bytes)

## Measuring

`dvbench` (`bench/`) times the engine on decoded pictures:
- per picture: plan + render + finish
- flat-out throughput
- real-time playback at the stream's clock

Extras: `PI_PROFILE=1` gives per-pass GPU times, and `PI_ABLATE=bits` (pack
pass only, for measurement) stubs out its parts:

| Bit | Stubs out |
|---|---|
| 1 | luma tables |
| 2 | EL gathers |
| 4 | chroma rows |
| 8 | SAND reads |
| 16 | the whole pass's work |
| 32 | tunnel packing |
| 64 | 4:2:2 filter |

On the Pi itself, `V3D_DEBUG=shaderdb` prints the compiler's statistics,
`V3D_DEBUG=fs` the QPU code, and `V3D_DEBUG=nir` the NIR.
`bundle/run-bench.sh` does it all per clip.

## Open

- **LibreELEC 13 integration:**
  - Vulkan enabled in our branch: `VULKAN="vulkan-loader"`; it is off for RPi
    by default.
  - The output dmabuf on a KMS plane (DRM PRIME).
  - The DV VSIF / HDMI signalling for the Pi's `vc4_hdmi`.
  - Kodi wiring.
- **Coverage:** Profile 5 (IPTPQc2) is untested. The Annex B EL upscale and
  resized output (the planner's `PACK_RESIZED`) aren't implemented.
