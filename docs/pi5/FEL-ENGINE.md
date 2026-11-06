# The Pi FEL engine: dual-layer Dolby Vision as Vulkan fragment passes

`bench/pi_engine.c` and `bench/pi/*.frag`, run through `dvbench --engines pi`.
This is the design from [V3DV.md](V3DV.md) §8 (two fragment passes in the end), built and measured on a
Raspberry Pi 5 (V3D 7.1 at 1150 MHz, Mesa 26.2.6, kernel 6.18.60, the HEVC
decoder's own SAND128 buffers).

## What it does

It does TV-led Dolby Vision, as yblod and CB1 do. The GPU reconstructs the
12-bit composer signal (BL + EL + RPU) and packs it, with the metadata
packets, into the tunnel frame. The TV does the display management. There is
no tone mapping on the Pi.

| Pass | Size | Reads | Writes |
|---|---|---|---|
| P1 | 1920×1080, one fragment per 4:2:0 chroma sample | BL and EL SAND dmabufs as texel buffers; the per-frame tables | reconstructed chroma (`RG16UI`) and the EL luma (`R16UI`) |
| P2 | 1920×1080, one fragment per 2×2 output block | P1's chroma (7 rows of its column), BL SAND luma (both rows), EL luma (two `textureGather`s), luma and residual tables, packets | the output dmabuf itself, twice: its even rows and its odd rows |

P2 fuses the tunnel's 4:2:2 chroma filter with the pack:
- The output dmabuf is imported as two linear `RG32UI` images: even rows at
  offset 0, odd rows at offset `stride`, both with a row pitch of
  `2 × stride`. They are P2's two colour attachments.
- Each fragment writes both rows of its pixel pair, so the chroma rows and
  the EL rows are fetched once for 4 pixels.
- The dmabuf needs one row of slack (`stride × (H + 1)`) for the odd-rows
  image's extent.

- **Each pass is one render pass, so one V3D render job.** There are no compute
  dispatches, storage writes or cache-clean jobs.
- **The CPU half is yblod's own planner** (`yblod/plan.c`, built into dvbench),
  so the tables, metadata packets and pack constants are yblod's. Planning
  takes 0.4–0.7 ms of CPU per frame.
- **Everything is integer-exact against yblod's bilinear FEL route except the
  chroma MMR, which has three modes:**

  | `--pi-mmr` | How |
  |---|---|
  | `float` | 32-bit float; coefficients and tables pre-scaled on the CPU |
  | `exact32` | exact, in 32-bit pieces: coefficient 16+16 bits × feature 10+10 bits, four 32-bit accumulators per channel, one 64-bit combine; feature powers rebuilt from 10-bit halves |
  | `exact64` | exact, yblod's way (`umulExtended` / `imulExtended`, 64-bit adds) |
- **Pipelines are specialised per stream:** chroma phase method, dual MMR, kinds
  and orders, and the 4:2:2 filter. Each is built the first time a stream
  needs it.

## Correctness (Pi, hardware decode)

`dvbench --engines vk,pi --el-scaler bilinear --compare` renders every
picture with yblod's engine and with this one, decodes both tunnel frames
back to 12-bit codes, and compares them.

| Clip | exact32 / exact64 | float |
|---|---|---|
| DV FEL All Layers Test (MMR) | identical: 0 of 99.5M luma, 0 chroma | identical |
| DV FEL Power Rangers Credits (MMR) | identical | 1,240 chroma samples differ (1,238 by 1, 2 by 2); luma identical |
| DV FEL Test DT DL P7 CMV4.0 (polynomial chroma) | identical | identical (no MMR) |

The metadata rows are covered by the comparison, so the packets are
identical too.

## Speed (Pi 5, 12 pictures, steady state, no throttling)

| Clip | exact64 | exact32 | float |
|---|---|---|---|
| CMV4.0 test (polynomial chroma) | 29.0 ms, **35.3 fps** | same | same |
| All Layers / Power Rangers (MMR) | 59.8 ms, 16.9 fps | 77.1 ms, 13.1 fps | 44.8 ms, **22.7 fps** |

For comparison, on the same pictures:

| Engine | ms per picture |
|---|---|
| yblod's Vulkan engine (compute) | 515 |
| CB1 Master Reference | 111 |
| CB1 Reference with bilinear FEL | 67 (about 43 of it CB1 itself) |

Per pass (`PI_PROFILE=1`), in ms:

| | P1 chroma | P2 4:2:2 + pack |
|---|---|---|
| no MMR | 11.5 | 17.0 |
| MMR float | 26.2 | 18.0 |
| MMR exact64 | 41.2 | 17.9 |
| MMR exact32 | 58.5 | 17.9 |

**Exact in 32-bit pieces against float:**
- **Float is much cheaper:** MMR costs about 15 ms per frame in float, 30 ms in
  exact64 and 47 ms in exact32.
- **exact32 is slower than exact64 on V3D.** It needs 4 split multiplies per
  term (each 32-bit `imul` is 2 instructions), and that costs more than the
  `umulExtended` emulation it replaces.
- **Exact mode is exact64, the default. Float is the fallback** when the
  hardware can't keep up: its error is at most 2 codes on about 0.001% of
  chroma samples.

## What the time was, and how it went

**The 4:2:2 and pack work: 42 → 17 ms**
1. **Separate P2 and P3 at 21 + 21 ms.**
   - P3 still cost 18 ms with every fetch removed. An empty 4K pass costs
     4 ms, linear or tiled.
   - P2 was texture-fetch bound: 7 row fetches per fragment.
2. **Fused into one 2×2-block pass: 31.8 ms.**
   - It saves P2's intermediate write and read, and its pass.
   - It shares the chroma and EL rows between the block's 4 pixels.
3. **No local arrays: 17.0 ms.**
   - V3D's compiler had put the 4:2:2 filter's row arrays (`vec2 r[7]`,
     written in a loop) into scratch memory before unrolling the loop.
   - Every value made a TMU store and load round trip. This is not reported
     as spills; the QPU dump shows it as `tmud`/`tmua` writes at the
     shader's start.
   - Writing the rows as separate variables removed it.
   - **Rule for every V3D shader here: no local arrays filled in loops.**

**Pass 1**
- 1,400 instructions at 2 threads became 4 threads once its selectors were
  specialisation constants.
- Sharing SAND words and the Cb/Cr tables halved its fetches.

**Things that didn't help**
- Rendering tiled and copying out costs 17 ms more than rendering into the
  linear dmabuf.
- `V3D_DEBUG=db` (double-buffered tiles) made no difference.
- A `textureGather` version of the 4:2:2 rows was slower (28 ms against 21).

`PI_ABLATE` (pack pass only, for measuring) stubs out parts of the pass:

| Bit | Stubs out |
|---|---|
| 1 | luma tables |
| 2 | EL gathers |
| 4 | chroma rows |
| 8 | SAND reads |
| 16 | the whole pass's work |
| 32 | tunnel packing |
| 64 | 4:2:2 filter |

## Next steps

1. **P1's MMR**, now the largest cost: 15 ms in float, 30 ms exact. The
   arrays in `mmr()` (`f0[4]`, `f1[4]`, `f[3]`) compile without scratch
   today, but they are the first thing to check against the QPU dump after
   any change.
2. **24p on MMR streams:**
   - Float is at 22.7 fps. About 3 ms more brings it to 24p.
   - Exact64 at 16.9 fps needs about 18 ms more.
3. **Real-time playback:** `dvbench --engines pi --play S`. The decoder alone
   runs at 58–64 fps for BL+EL.
4. **On LibreELEC 13:**
   - Vulkan enabled in our branch (`VULKAN="vulkan-loader"`).
   - The output dmabuf (one row of slack) on a KMS plane (DRM PRIME).
   - The DV VSIF / HDMI side for the Pi's `vc4_hdmi`.
