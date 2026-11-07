# The engine

Dolby Vision for the Raspberry Pi 5, as two Vulkan fragment passes on the V3D
GPU (`engine/`).

It is TV-led, as in Kodi's DV engines for other platforms:
1. It reconstructs the composer's 12-bit signal from the base layer (BL), the
   enhancement layer (EL) and the RPU.
2. It packs that signal, with the display-management metadata, into the HDMI
   "tunnel" frame: RGB 8-bit carrying 12-bit 4:2:2, at the stream's size
   (3840×2160 or 1920×1080).
3. The TV does the display mapping. There is no tone mapping on the Pi.

## Pipeline

```
HEVC decoder (V4L2 request) ── BL, EL: SAND128 P030 dmabufs ──┐
RPU (AVDOVIMetadata) ── planner (CPU, ~0.5 ms) ── tables, packets ─┤
                                                                  ▼
P1  w/2 × h/2   chroma: BL chroma phase, luma guide, EL chroma upsample,
                MMR / polynomial / piecewise chroma mapping + NLQ residual
                → RG16UI chroma (12-bit), R16UI EL luma
P2  w/2 × h/2   one fragment per 2×2 output block: 4:2:2 vertical filter,
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

## Using the engine

`engine/pi_engine.h` is the API, for a long-lived caller such as Kodi:

| Call | When |
|---|---|
| `pi_engine_create(&options, &e)` | once: device, MMR mode, filters, pipeline cache file, log callback |
| `pi_stream_open(e, &info)` | per stream: its size and profile. Resets the planner (packet ids, cached tables). |
| `pi_output_import(e, fd, w, h, stride, size, &out)` | per scanout buffer the caller owns; `pi_output_size()` gives the layout |
| `pi_render(e, &frame, out, &release_fd)` | per picture. Returns a sync_file signalled when `out` is complete; `frame.acquire_fd` is one the output is busy until (KMS's). |
| `pi_output_done` / `pi_output_wait` | without sync_files |
| `pi_stream_close`, `pi_output_release`, `pi_engine_destroy` | release everything, in reverse |

- **Up to three renders in flight.** `pi_render` waits for the oldest beyond
  that. The engine holds references to the BL and EL frames until the GPU is
  done with them.
- **Errors a caller can act on:** `PI_E_UNSUPPORTED_FORMAT` (not a SAND128
  frame of the stream's size), `PI_E_UNSUPPORTED_PLAN` (the planner refuses
  the stream, for example single-layer MMR under CB1's filters),
  `PI_E_BAD_RPU`, `PI_E_VULKAN`, `PI_E_NOMEM`.
- **Decoder buffers** are imported once per stream and released by
  `pi_stream_close`.
- **Pipelines** are cached on disk when `pipeline_cache` names a file. After
  a stream's first frame, a worker thread at idle priority builds the
  variants the stream may need next (the MMR's other orders, the split's
  guard), so a scene change doesn't wait for a compile.
- **Queue ownership** of the decoder buffers and the output moves to and
  from `VK_QUEUE_FAMILY_FOREIGN_EXT` around each render.
- **The release sync_file is the render's fence, exported** (where the
  device has `VK_KHR_external_fence_fd`; the engine then tracks the render
  by its own copy, since exporting resets the fence). A semaphore signalled
  by the submission costs v3dv the overlap of consecutive frames: 1917
  dropped 0–5 frames in 120 s with one, none with the fence.
- **An acquire fence already signalled isn't imported,** for the same reason
  on the waiting side.
- **Give KMS the sync_file (`IN_FENCE_FD`).** v3d does put an implicit fence
  on the output dmabuf, and KMS waits on it. But it appears only once v3dv
  has passed the job to the kernel, after `vkQueueSubmit` returns
  (`dvbench --implicit-sync`, kernel 6.18.60: no fence right after submit
  in 12 of 12 pictures, a fence 2 ms later in all 12). A commit inside that
  window would show an unfinished frame. The sync_file has no such window.
  `dvbench --kms-writeback` checks the whole path without a display: the
  output on a KMS plane with `IN_FENCE_FD`, captured by a writeback
  connector, is the finished frame (12 of 12, 1080p, a 46 ms render
  committed while still drawing).
- **Input glue** for a player (`engine/pi_input.c`): `pi_chroma_top_left()`,
  `pi_is_sand128()` and `pi_p7_split()`, the single-track Profile 7 BL / EL
  split. `bench/source.c` is the reference for decoding and pairing the
  layers, by display time (`el_display_times()`: an EL picture's own
  timestamp is its packet's, not its display time, where the EL's coding
  order differs).
- **Build:** `libpi_engine.a` and `libdv_planner.a`, with `pi_engine.h`,
  install with CMake. glslc is optional: without it the committed SPIR-V
  (`engine/shaders/spirv/`, built with `-O`, which V3D needs: 7–9% faster)
  is used; `cmake --build . --target update-spirv` refreshes it. dvbench
  builds with `-DPI_BUILD_BENCH=ON`.
- **Licence:** GPL-2.0-or-later (`LICENSE`), as Kodi; third-party notices
  in `engine/planner/LICENSE`. The bench's FFmpeg is built
  `--enable-version3`, so binaries linking it are GPLv3 as a whole, which
  "or later" allows. Kodi's FFmpeg must carry `patches/ffmpeg` (the Dolby Vision RPU
  extensions the planner reads, `DV_FFMPEG_RAW_EXTENSIONS`).

## Routes

The CPU half, the planner in `engine/planner/` (vendored from yblod), picks a
route per frame. The passes are specialised per stream: the route, chroma
phase method, MMR kinds and orders, and the 4:2:2 filter. Each pipeline is
built the first time a stream needs it.

| Route | Streams | P1 chroma | Luma |
|---|---|---|---|
| dual | Profile 7 FEL | MMR (or polynomial) + EL residual | mapping table + bilinear EL + NLQ |
| single | Profile 7 MEL, 8.1 with polynomial chroma | 12-bit tables | 12-bit table |
| piecewise | Profile 8.1 with MMR chroma (`--filters phase`), Profile 8.4 | one MMR piece per channel: the MMR in the `--mmr` mode (exact unless `float`); otherwise yblod's general reshape (exact, slow) | 12-bit table |

**Spatial filters** (`--filters`):
- **`cb1`, the default:** CB1 Master Reference's contract. Reshaping at 4:2:0,
  no chroma phase filter, Annex B 4:2:2 filters.
- **`phase`** (also `yblod`): YBLOD's chroma phase filters, the 2-tap 4:2:2
  blend, and the full-height 4:2:2 layout for single-layer MMR. The planner
  refuses single-layer MMR under CB1's contract.

**The FEL EL upscale is bilinear.** Annex B is the Master Reference filter;
it is not implemented in the passes.

## Exactness

The output is bit-exact against yblod's Vulkan engine with the bilinear EL
upscale: every sample of the tunnel frame, metadata included.
- **How it was proven:** sample by sample, on every clip and film point
  tested, before yblod was removed from this repository.
- **What keeps it:** `tests/golden/hashes.txt` carries those results.
  `tests/check.sh` must reproduce them.
- **MMR curves beyond the 64-bit LUT** (the planner's MMR96 / MMR128 kinds)
  are computed in emulated 128-bit in every exact mode. No stream tested so
  far has them; `--measure force_wide` runs a 64-bit stream through that
  path, and its golden case must equal the stream's own.
- **1080p** matches yblod's render of the same pictures (centred in a 4K
  canvas: its engine can't render native 1080p), except the metadata
  packets' LSBs, which our frame carries in its own first rows.

## Chroma MMR modes (`--mmr`)

MMR is a 3-variable polynomial of up to 3rd order. Computed exactly it needs
64-bit integer arithmetic, which V3D doesn't have.

| Mode | How | Exact | 1917 |
|---|---|---|---|
| `exact`, the default | Float, with a per-sample error bound (see below). Samples whose bound straddles a 12-bit step take `split`. | yes | 35.2 ms |
| `split` | The 64-bit sum's low 32 bits from wrapping 32-bit integer multiply-adds, its high 32 bits from a float sum: `H = round(y − L / 2³²)`. The CPU bounds float's error for the frame's largest magnitudes: 32 × 2⁻²⁴ × Σ max\|terms\| < 0.5 (1917: 0.01; Sol Levante 8.4: up to 0.46). A frame over it checks each sample's own magnitudes instead, and takes `exact64` where that fails too (the guard, about 2 ms). | yes | 33.1 ms |
| `exact64` | yblod's emulated 64-bit (`umulExtended`) | yes | 45.8 ms |
| `float` | 32-bit float | within 2 codes, about 0.001% of samples | 28.9 ms |

`exact32` (16-bit coefficient halves) was slower than `exact64` and is gone.

**The `exact` bound.** Vulkan's add and multiply are correctly rounded, so
the float value lies within 20 × 2⁻²⁴ × Σ|terms| of the exact one, with the
feature powers computed exactly. About 4.6% of 1917's chroma samples fall
inside that bound of a 12-bit step and take the exact path.

## Performance

Raspberry Pi 5, stock 1150 MHz GPU, no cooler, Mesa 26.2.6, hardware decode,
read over SMB:

| Film | Route | Per picture | 24p real time |
|---|---|---|---|
| 1917 (UHD BD, P7.6 FEL, MMR, 2.39:1) | dual, `exact` | 33.0 ms (30.3 fps; 35.2 before the review) | with sync_file fences, 4 × 120 s: 0, 0, 0 and 1 late (≤ 0.8 ms); without, 0, 1 and 0; 73 °C, no throttling |
| Dune (2021) (UHD BD, P7.6 MEL) | single | 16.1 ms (65 fps) | 60 s, 0 late |
| Profile 8.1 MMR cuts (1917, test clips) | piecewise | 26.1–27.9 ms | 8 s, 0 late |
| DV FEL test clips (polynomial chroma) | dual | 29.0 ms (35 fps) | — |
| Sol Levante (Dolby's samples), Profile 5 | single | 18.4 ms (54 fps) | 90 s, 0 late |
| Sol Levante, Profile 8.1 | single | 18.3 ms (55 fps) | 90 s, 0 late |
| Sol Levante, Profile 8.4 (HLG base) | piecewise, `exact` | 30.8 ms (32 fps); 43.5–53.4 ms with `--filters phase` | whole film (263 s, 6,314 pictures), 1 late (1.9 ms), 68 °C, no throttling |
| Sol Levante 1080p, Profiles 5 and 8.1 | single | 4.7 ms (212 fps) | 60 s, 0 late |

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
   as a fragment pass than as compute, in Vulkan and GLES alike (measured with
   the since-removed `vk-micro`; `pi5/V3DV.md`). Vulkan and GLES fragment
   passes run at the same speed.
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
   35.2 ms (bounded). The exception is `exact64`, now only the split's
   rare fallback: unrolled, the guarded variant spilled (92 spills against 6).
   Reads past a plane's edge are avoided by reading the last column with its
   left neighbour, not by clamping each read: the clamps cost 1.3 ms on 1917.
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

Other switches, for debugging and measurement. dvbench reads them and sets
`pi_options` (`log_level`, `profile`, `measure`, `ablate`); the engine itself
reads no environment. `--measure` takes the same names, comma-separated.

| Variable | `--measure` | Effect |
|---|---|---|
| `PI_DEBUG=1` | | log the plan, the tables and the pipeline builds |
| `PI_ALL_ROWS=1` | `all_rows` | no letterbox skipping: both passes over every row |
| `PI_DRAW_BARS=1` | `draw_bars` | draw the bars instead of clearing them |
| `PI_PIECE_GENERAL=1` | `piece_general` | piecewise route: always the general reshape |
| `PI_MARK=1` | `mark` | write Q16 65535 / 0 (12-bit 4095 / 0) where `exact` fell back; 0 / 65535 where the split's guard did |
| `PI_COPY_OUTPUT=1` | `copy_output` | render to an image and copy it to the dmabuf, as on devices without linear DRM-modifier attachments (lavapipe) |
| `PI_FORCE_WIDE=1` | `force_wide` | compute 64-bit LUT MMRs in the 128-bit path |
| `PI_DEVICE=NAME` | | Vulkan device (as `dvbench --device`) |
| `DVBENCH_ALLOC=udmabuf\|cma\|system` | | the bench's dmabuf allocator (default: the first that works) |

`dvbench --probe` checks the allocator, the device and an output import.
`--no-fences` makes `--play` wait on the engine instead of the sync_files.
`--implicit-sync` reports whether the output dmabuf carries an implicit
fence right after each submit and 2 ms later. `--kms-writeback` shows each
output on a KMS plane with its sync_file as `IN_FENCE_FD`, captured by a
writeback connector, against the finished frame, then the same without the
fence (`DVBENCH_KMS_DEBUG=1`: commit timings). The Pi's writeback offers no
3840×2160 mode, so it needs a 1080p stream. `--reopen` closes and reopens
the stream in the same engine and requires identical output (the planner
reset; without it no picture matches). `DVBENCH_DROP_EL=N` discards the Nth EL picture
(the BL / EL pairing's test).

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
- **Integration review:** `docs/REVIEW.md` lists what the engine needs
  before it runs inside Kodi.
- **Coverage:**
  - Profiles 5, 8.1 and 8.4 were checked on Dolby's Sol Levante samples
    (`DolbyLaboratories/dolby-vision-contents`). At 4 points through the film,
    they are identical to yblod's engine, Profile 8.4 with `--filters yblod`.
    yblod refuses 8.4 under `cb1`, so that output has no reference; there,
    `exact`, `split` and `exact64` agree on V3D at the same 4 points.
  - Profile 8.4 is real time with the default `cb1` filters only.
  - 1080p (Profiles 5, 8.1, 8.4) is checked as above; Profile 7 at 1080p
    has no test clip.
  - The Annex B EL upscale and resized output (the planner's
    `PACK_RESIZED`) aren't implemented.
