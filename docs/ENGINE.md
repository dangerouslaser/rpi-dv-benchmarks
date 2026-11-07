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
| `pi_probe(&options, &info)` | for a capability check: the device `pi_engine_create` would use, without creating an engine |
| `pi_engine_create(&options, &e)` | once: device, MMR mode, filters, EL upscale, pipeline cache file, log callback |
| `pi_engine_configure(e, &options)` | a change of MMR mode, filters or EL upscale (a player's settings), from the next `pi_stream_open` |
| `pi_stream_open(e, &info)` | per stream: its size and profile. Resets the planner (packet ids, cached tables). |
| `pi_output_import(e, fd, w, h, stride, size, &out)` | per scanout buffer the caller owns; `pi_output_size()` gives the layout |
| `pi_render(e, &frame, out, &release_fd)` | per picture: its BL and EL frames. Returns a sync_file signalled when `out` is complete; `frame.acquire_fd` is one the output is busy until (KMS's). |
| `pi_output_done` / `pi_output_wait` | without sync_files |
| `pi_stream_close`, `pi_output_release`, `pi_engine_destroy` | release everything, in reverse |

- **The RPU is the frames' side data,** as FFmpeg's HEVC decoder attaches
  it: `AV_FRAME_DATA_DOVI_METADATA`, and `AV_FRAME_DATA_DOVI_RPU_BUFFER`,
  the raw RPU, from which the CM v4.0 extension blocks (L3, L8–L11, L254) are
  re-encoded (FFmpeg's parse doesn't keep their bytes). The BL frame's, else
  the EL's; `frame.rpu_frame` names another (an EL frame not rendered). A
  picture with CM v4.0 blocks and no RPU buffer is refused
  (`PI_E_UNSUPPORTED_PLAN`), unless its metadata repeats the last RPU's,
  as FFmpeg's does for an access unit without one.
- **Up to three renders in flight.** `pi_render` waits for the oldest beyond
  that. The engine holds references to the BL and EL frames until the GPU is
  done with them.
- **Errors a caller can act on:** `PI_E_UNSUPPORTED_FORMAT` (not a SAND128
  frame of the stream's size), `PI_E_UNSUPPORTED_PLAN` (the planner refuses
  the stream, for example single-layer MMR under CB1's filters),
  `PI_E_BAD_RPU` (no RPU, or an invalid one), `PI_E_VULKAN`, `PI_E_NOMEM`.
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
- **Give KMS the sync_file (`IN_FENCE_FD`): implicit sync doesn't hold.**
  `dvbench --kms-writeback` puts each output on a KMS plane, committed while
  the GPU still draws it, and captures what KMS scans out with the vc4
  writeback connector (no display needed). The output is pre-filled with a
  marker. At 3840×2160:
  - with `IN_FENCE_FD`, every capture is the finished frame (1917 12 of 12;
    a 46 ms 8.4 render 8 of 8);
  - without it, the commit doesn't wait: every capture shows the unfinished
    marker (12 of 12, 8 of 8).

  v3d's implicit fence on the output does exist, but only once v3dv has
  passed the job to the kernel, a moment after `vkQueueSubmit` returns
  (`dvbench --implicit-sync`: none right after submit, 12 of 12; one 2 ms
  later, 12 of 12). At 1080p a bare commit happened to wait on it, at 4K it
  didn't.
- **Input glue** for a player (`engine/pi_input.c`): `pi_chroma_top_left()`,
  `pi_is_sand128()`, `pi_p7_split()` (the single-track Profile 7 BL / EL
  split), `pi_frame_needs_el()` and `pi_pairer`:
  - `pi_frame_needs_el()`: whether a picture's RPU has a residual for the
    EL (FEL), by the planner's own test. MEL's residual is zero, so a player
    may close the EL decoder.
  - `pi_pairer` pairs the two decoders' pictures by display time: the BL
    picture's timestamp, and the EL picture's as its place among the EL
    decoder's output gives it (an EL picture's own timestamp is its
    packet's, not its display time, where the EL's coding order differs).
    A picture whose partner a decoder lost: an EL is dropped, a BL that
    needs its EL is dropped, one that doesn't is given alone. An EL decoder
    that gives no pictures by the 8th BL is given up (BLs alone). Flushed on
    a seek.
  `bench/source.c` is the reference for decoding the layers around them.
- **Build:** `libpi_engine.so.1` (the SONAME follows `PI_API_VERSION`;
  `pi_api_version()` is the library's), exporting the `pi_*` API only
  (`engine/pi_engine.map`), with `pi_engine.h` and `pi_engine.pc`
  (requires `libavutil`); `-DPI_SHARED=OFF` builds `libpi_engine.a` and
  `libdv_planner.a` instead. It needs Vulkan and libavutil. glslc is
  optional: without it the committed SPIR-V (`engine/shaders/spirv/`, built
  with `-O`, which V3D needs: 7–9% faster) is used;
  `cmake --build . --target update-spirv` refreshes it. dvbench builds with
  `-DPI_BUILD_BENCH=ON`. `scripts/make-release.sh` makes the source release
  (`out/pi-dv-engine-<version>.tar.xz`, HEAD, the version in
  `CMakeLists.txt`) and prints its SHA-256.
- **Licence:** GPL-2.0-or-later (`LICENSE`), as Kodi; third-party notices
  in `engine/planner/LICENSE`. The bench's FFmpeg is built
  `--enable-version3`, so binaries linking it are GPLv3 as a whole, which
  "or later" allows. The engine needs no FFmpeg patches: it reads the side
  data upstream FFmpeg's HEVC decoder attaches.

## Display output

What the HDMI output must carry for a TV to show the tunnel frame as Dolby
Vision (`dvbench --kms-play` sets all of it):
- **The stream's size and frame rate** (3840×2160 at 23.976 Hz for a UHD
  Blu-ray), no scaling.
- **RGB, full range, 8 bits per channel:** the connector's `Broadcast RGB` =
  Full and `max bpc` = 8. Limited range would rewrite the packed bits.
- **A Dolby Vision vendor infoframe:** the HDMI 1.4 VSIF (OUI 00-0C-03)
  padded to 24 bytes, or Dolby's (OUI 00-D0-46). The Raspberry Pi kernel has
  no way to send one; `patches/linux/` adds the HDMI connector property
  `Dolby Vision` (`Off`, `Standard (HDMI VSIF)`, `Standard (Dolby VSIF)`),
  which the player sets with the mode and sets back to `Off` after. Without
  it the TV shows the frame as SDR RGB. A TV may stay in Dolby Vision when
  the infoframe just stops; `Off` sends the ordinary one.
- **The output behind its sync_file** (`IN_FENCE_FD`), as above.

On an LG B4 (webOS 9.1), the TV then reports `hdrType` DolbyVision for its
HDMI input (`com.webos.service.videooutput/getStatus`), and its own captures
of the screen show the correct pictures. Checked: Profile 7 FEL (the three
DV FEL test clips), 5 and 8.1 at 4K and 1080p, 8.4, and 8.1 MMR, each at the
stream's full rate.

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

**The FEL EL upscale** (`pi_options.el_upscale`, `--el-upscale`), at the same
sample positions in every option:
- **`bilinear`, the default:** vertical 3:1, horizontal 1:1, as yblod's
  engine.
- **`annexb`:** ETSI GS CCM 001 Annex B's example filter, the Master
  Reference's: the EL luma through a vertical 4-tap (−3 29 111 −9 / 128 and
  its mirror) and the 8-tap horizontal (22 94 −524 2456 2456 −524 94 22 /
  4096) on the odd columns; the EL chroma through the 64/192 vertical
  filter (both upscales') and the same 8-tap. Pass 2 reads 12 gathers per
  fragment for it, pass 1 four EL column pairs on odd columns.
- **`bicubic`:** Catmull-Rom at the same sample positions, luma and chroma:
  vertical −9 111 29 −3 / 128 and its mirror (Annex B's luma vertical),
  horizontal −1 9 9 −1 / 16 on the odd columns. Pass 2 reads 6 gathers per
  fragment; pass 1 four EL rows per column.
- **`lanczos`:** Lanczos-3 at the same positions, luma and chroma, its
  weights normalised to 1024: vertical 8 −70 277 914 −136 31 and its mirror,
  horizontal 25 −139 626 626 −139 25 on the odd columns. Pass 2 reads 12
  gathers per fragment; pass 1 six EL rows per column.

## Exactness

The output is bit-exact against yblod's Vulkan engine with the bilinear EL
upscale: every sample of the tunnel frame, metadata included.
- **How it was proven:** sample by sample against yblod's engine, on every
  clip and film point tested.
- **What keeps it:** `tests/golden/hashes.txt` carries those results.
  `tests/check.sh` must reproduce them.
- **MMR curves beyond the 64-bit LUT** (the planner's MMR96 / MMR128 kinds)
  are computed in emulated 128-bit in every exact mode. No test stream has
  them; `--measure force_wide` runs a 64-bit stream through that path, and
  its golden case must equal the stream's own.
- **The Annex B EL upscale** is identical to yblod's engine with its CB1
  option (YBLOD 0.3.2 `--cb1`, which upscales the EL by Annex B), frame
  for frame. Its upscaled EL alone (luma and chroma, exposed by `--measure
  el_out` with `PI_ABLATE=33`) is identical, sample for sample, to the
  planner's CPU reference (`pixel_spatial.c` `dv_annex_b_scale2x`) under
  both filter sets. The golden cases with `--el-upscale annexb` keep that.
- **The bicubic and Lanczos EL upscales** have no outside reference: each
  one's upscaled EL is identical to a separate C implementation of the
  filter under both filter sets, and their golden cases keep that.
- **1080p** matches yblod's render of the same pictures (centred in a 4K
  canvas: its engine can't render native 1080p), except the metadata
  packets' LSBs, which our frame carries in its own first rows.

## Chroma MMR modes (`--mmr`)

MMR is a 3-variable polynomial of up to 3rd order. Computed exactly it needs
64-bit integer arithmetic, which V3D doesn't have.

| Mode | How | Exact | 1917 |
|---|---|---|---|
| `exact`, the default | Float, with a per-sample error bound (see below). Samples whose bound straddles a 12-bit step take `split`. | yes | 33.0 ms |
| `split` | The 64-bit sum's low 32 bits from wrapping 32-bit integer multiply-adds, its high 32 bits from a float sum: `H = round(y − L / 2³²)`. The CPU bounds float's error for the frame's largest magnitudes: 32 × 2⁻²⁴ × Σ max\|terms\| < 0.5 (1917: 0.01; Sol Levante 8.4: up to 0.46). A frame over it checks each sample's own magnitudes instead, and takes `exact64` where that fails too (the guard, about 2 ms). | yes | 32.9 ms |
| `exact64` | yblod's emulated 64-bit (`umulExtended`) | yes | 53.1 ms |
| `float` | 32-bit float | within 2 codes, about 0.001% of samples | 30.8 ms |

**The `exact` bound.** Vulkan's add and multiply are correctly rounded, so
the float value lies within 20 × 2⁻²⁴ × Σ|terms| of the exact one, with the
feature powers computed exactly. About 4.6% of 1917's chroma samples fall
inside that bound of a 12-bit step and take the exact path.

## Performance

Raspberry Pi 5, stock 1150 MHz GPU, no cooler, Mesa 26.2.6, hardware decode,
read over SMB:

| Film | Route | Per picture | 24p real time |
|---|---|---|---|
| 1917 (UHD BD, P7.6 FEL, MMR, 2.39:1) | dual, `exact` | 33.0 ms (30.3 fps) | 120 s with sync_file fences, 0–1 late (≤ 0.8 ms); 73 °C, no throttling |
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
   as a fragment pass than as compute, in Vulkan and GLES alike
   (`pi5/V3DV.md`). Vulkan and GLES fragment passes run at the same speed.
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
| `PI_EL_OUT=1` | `el_out` | the dual route's pass 1 writes the upscaled EL chroma as its chroma (with `PI_ABLATE=33`: the EL upscale's test) |
| `PI_DEVICE=NAME` | | Vulkan device (as `dvbench --device`) |
| `DVBENCH_ALLOC=udmabuf\|cma\|system` | | the bench's dmabuf allocator (default: the first that works, in the order udmabuf, system, cma: CMA is the Pi decoder's) |
| `DVBENCH_DV_VSIF=hdmi\|dolby` | | `--kms-play`'s `Dolby Vision` value: the HDMI VSIF (default) or Dolby's |

`dvbench --probe` checks the allocator, the device and an output import.
`--no-fences` makes `--play` wait on the engine instead of the sync_files.
`--implicit-sync` reports whether the output dmabuf carries an implicit
fence right after each submit and 2 ms later. `--kms-writeback` shows each
output on a KMS plane with its sync_file as `IN_FENCE_FD`, captured by a
writeback connector, against the finished frame, then the same without the
fence (`DVBENCH_KMS_DEBUG=1`: commit timings; a mode of the stream's size
is made when the connector doesn't list one). `--selftest` checks the API's
behaviour on the decoded pictures: error codes, input and output
validation, the engine's references to the frames, the pipeline cache's
growth, imports released at stream close, teardown with renders in flight.
`tests/check.sh` runs it (under the validation layer with `--validate`);
`tests/check.sh --gpuav` adds Vulkan's GPU-assisted validation, which reports
out-of-bounds buffer reads in the shaders, on every route and filter set. `--reopen` closes and reopens
the stream in the same engine and requires identical output (the planner
reset; without it no picture matches). `DVBENCH_DROP_EL=N` discards the Nth EL picture
(the BL / EL pairing's test: a FEL clip loses that BL and every other
picture matches the run without it).

On the Pi itself, `V3D_DEBUG=shaderdb` prints the compiler's statistics,
`V3D_DEBUG=fs` the QPU code, and `V3D_DEBUG=nir` the NIR.
`bundle/run-bench.sh` does it all per clip.

## Limitations

- **Not implemented:** resized output (the planner's `PACK_RESIZED`: the
  tunnel frame is the stream's size).
- **`--filters phase` on single-layer MMR** isn't real time at 24p: it
  reshapes both output rows' chroma, twice pass 1's MMR. Profile 8.4 takes
  43–53 ms per picture, an 8.1 MMR cut of 1917 66 ms. With the default
  filters both are real time (31 and 27 ms).
- **Coverage:** Profiles 5, 8.1 and 8.4 are checked on Dolby's Sol Levante
  samples (`DolbyLaboratories/dolby-vision-contents`), identical to yblod's
  engine at 4 points through the film (8.4 with `--filters phase`; under
  `cb1` yblod refuses 8.4, and there `exact`, `split` and `exact64` agree on
  V3D). No Profile 7 1080p stream has been tested.
