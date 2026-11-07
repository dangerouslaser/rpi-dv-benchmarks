# Code review: before LibreELEC integration

A review of the whole repository (except the vendored planner and FFmpeg
patches), November 2026. So far the engine has only run inside `dvbench`,
which runs once and exits. Inside Kodi it will run for hours, through new
files, seeks and stream changes. Most findings are about that difference.
Line numbers are as of commit `ff53c4e`.

Severity:
- **P0:** breaks real playback in Kodi.
- **P1:** needed for integration.
- **P2:** hardening.
- **P3:** debt and cleanup.

Strike items out here as they are fixed.

## P0: breaks playback in a long-lived engine

1. **Rebinding an output slot renders into the old buffer**
   (`engine/pi_engine.c:560`). `output_half` makes a new image, but it only
   makes a view `if (!o->view)`. The framebuffer is therefore built from the
   old image's view. The old image, memory, framebuffer and `out_buf` leak.
   There is also no wait before a slot that may still be busy is rebound.
   - **Fix:** wait on the slot, destroy the old objects, then rebuild.
2. **The pipeline caches are fixed in size, and a full one fails every later
   frame** (`:112`, `:869`, `:1048`).
   - P2's cache has 8 entries, and P2's key alone has up to 12 combinations
     plus the bars pipeline. P1's cache has 16, and P1's key includes
     per-scene MMR kinds and orders.
   - Once a cache is full, `variant()` returns NULL and `pi_submit` fails for
     the life of the engine.
   - **Fix:** a growable cache, and a `VkPipelineCache` kept on disk (P1 #4).
3. **Decoder imports are never released** (`:200-248`). The import list only
   grows. Each import keeps a decoder dmabuf, which is CMA memory, alive. The
   V4L2 decoder allocates new buffers on each reinitialisation, so every file
   leaks CMA until allocation fails.
   - **Fix:** release the imports per stream (`pi_stream_close`).
   - **Note:** identifying a buffer by its inode (`:207`) is only safe while
     the engine holds a reference to it.
4. **The input frames' lifetime is undefined.** The GPU reads BL and EL after
   `pi_submit` returns. If the caller frees its `AVFrame` first, the decoder
   can recycle the buffer while the GPU is still reading it.
   - **Fix:** take `av_frame_ref` on BL and EL per slot, and release them in
     `pi_wait`/`pi_done`.
5. **`pi_destroy` leaks everything but the device and instance**
   (`:1129-1135`; "the process exits right after"). Destroying a device that
   still has live objects is invalid Vulkan. A `create` that fails part-way
   goes through the same path (`:519`).
   - **Fix:** full teardown in reverse order, safe to call on a partial
     create.

6. **Done: Profile 8.4 is real time.** It took 184–298 ms per picture,
   because the fast piecewise path needed the split MMR and the split's
   error check (64 × 2⁻²⁴ × m < 0.25) refused this stream.
   - The fast path now runs the stream's `--mmr` mode.
   - The check is now 32 × 2⁻²⁴ × m < 0.5, from a count of the roundings.
   - A frame that still fails checks each sample (`SPLIT_GUARD`) instead of
     dropping to `exact64` whole. Both variants are built together.
   - Result: 30.5–35.5 ms, and the whole film plays with 1 late frame.
   - Still to do: an 8.4 golden case in a regression script.

## P1: what integration needs

1. **An API for a long-lived caller.** The bench drives the engine through
   3 fixed slots. Kodi needs:
   - stream open and close (which also resets the planner state: K5 below)
   - outputs it owns and imports (validated)
   - fences
   - error codes it can tell apart

   A sketch:
   ```c
   int  pi_engine_create(const struct pi_options *, struct pi_engine **);
   void pi_engine_destroy(struct pi_engine *);
   int  pi_stream_open(struct pi_engine *, const struct pi_stream_info *); // size, profile, siting; prewarm
   void pi_stream_close(struct pi_engine *);                                // wait, release imports and frames
   int  pi_output_import(struct pi_engine *, int fd, uint32_t stride, uint64_t size, struct pi_output **);
   void pi_output_release(struct pi_engine *, struct pi_output *);
   int  pi_render(struct pi_engine *, const AVFrame *bl, const AVFrame *el, const AVFrameSideData *dovi,
                  struct pi_output *, int acquire_fd, int *release_fd);
   enum pi_error { PI_OK, PI_E_UNSUPPORTED_FORMAT, PI_E_UNSUPPORTED_PLAN, PI_E_BAD_RPU, PI_E_VULKAN, PI_E_NOMEM };
   ```
   `bench/dvbench.c` `play()` (`:239-295`) is the reference for the
   submit/wait/late loop.
2. **Sync with the display plane.** The only sync today is the CPU waiting.
   - Export a sync_file (`VK_KHR_external_fence_fd`) for KMS `IN_FENCE_FD`.
   - Take an acquire fence for buffers KMS may still be scanning out.
   - Whether v3d's implicit `dma_resv` fences are enough on their own is
     unverified.
   - Queue-family ownership transfers to `VK_QUEUE_FAMILY_FOREIGN_EXT` are
     missing (`:261`, `:435`). This is a spec gap that is probably harmless
     on v3dv.
3. **Sizes other than 3840×2160.** The size is hard-coded (`:440`, `:891`).
   Confirmed: Dolby's 1080p samples of Profiles 5, 8.1 and 8.4 all return 2.
   Profile 8.1 at 1080p, which is common in streaming, is refused. The refusal
   uses the same return code (2) as "the planner can't do this", so the
   caller can't tell the two apart.
4. **Pipelines compile on the render thread** (`:875`), and P1's key changes
   with the scene's MMR order.
   - Prewarm the pipelines at stream open.
   - Pass a `VkPipelineCache` saved under Kodi's userdata (today it is
     `VK_NULL_HANDLE`, `:327`).
5. **K5: planner state across streams.** `dvvk_plan_state_reset` is never
   called, so packet-id continuity and cached tables carry over between
   files. Reset at stream open.
6. **Logging and options.**
   - One `e->warned` counter silences every warning after the first
     (`:909`, `:972`, `:1014`).
   - `getenv` runs per frame (`PI_DEBUG`, `PI_ALL_ROWS`, `PI_MARK` and
     others), and `static int shown` (`:670`) is global to the process.
   - Errors go to stderr, which Kodi doesn't capture.
   - **Fix:** a `pi_options` struct at create, a log callback, and one flag
     per warning.
7. **Packaging.**
   - `glslc` is required. Either commit the generated SPIR-V headers, or
     confirm LibreELEC's host glslang can build them.
   - `bench/` always builds and pulls in EGL/GLES: add
     `option(PI_BUILD_BENCH OFF)`.
   - Add `install()` rules and a header for Kodi.
   - **Licence:** the planner is GPL-3.0-or-later, so a Kodi binary linking
     it is effectively GPLv3. Kodi is GPL-2.0-or-later, so this is allowed,
     but it has to be stated. The repository root has no LICENSE file.
   - Kodi's FFmpeg in LibreELEC must carry `patches/ffmpeg`
     (`DV_FFMPEG_RAW_EXTENSIONS`).
8. **Input glue to move next to the engine** (from `bench/source.c` and
   `dvbench.c`):
   - the single-track Profile 7 BL/EL split (`source.c:226-302`), and the
     pairing of BL and EL pictures
   - the chroma siting policy `top_left()` (`dvbench.c:148`)
   - the SAND128 modifier check, duplicated in `pi_engine.c:665`
   - the output size rule, as `pi_output_size()` (`dvbench.c:75-85`)

## P2: hardening

1. **The input frame isn't checked** (`pi_engine.c:662`). `data[0]` is
   treated as an `AVDRMFrameDescriptor` without checking the frame's format,
   and `object_index` isn't bounds-checked (`:683`). A software-decode
   fallback in Kodi would make the engine read pixel bytes as fds.
2. **Unvalidated indices and buffers.**
   - Slot indices are never bounds-checked (`:571`, `:887`, `:1095`,
     `:1113`, `:1119`).
   - The output's stride, alignment and size (`stride*(h+1)`) aren't
     validated (`:532`).
3. **Shader reads past the plane at the right edge.**
   - `sand4` (`p1_chroma.frag:69-74`) always fetches word `w+1`. At the last
     chroma column, that word lies past the end of the BL chroma plane, and
     of the EL plane on every dual frame.
   - The same happens in `sand2(el_y)` (`:439`) and in the `PIECE==2` guide
     (`:427`).
   - The values are discarded, but without robust buffer access the read is
     undefined behaviour.
   - **Fix:** fetch the second word only when it's needed, or clamp the
     index. Either way the output stays bit-identical.
4. **MMR96 and MMR128 streams render in float, not exactly**
   (`pi_engine.c:970`). The planner emits these kinds for MMR curves that
   don't fit the 64-bit LUT (`planner/plan.c:316-332`). Every test clip and
   film so far has used the LUT kind.
   - **Fix:** route them through `exact64` and give them a golden case.
5. **`dvbench` accepts unknown option values as the default**
   (`dvbench.c:331`, `:343`, `:346-350`). `--mmr exat` runs `exact`, and a
   bad `--mmr` in `tests/check.sh` would compare exact against exact and
   pass. Reject unknown values.
6. **Decoder feed** (`bench/source.c`). This is bench code, but it is the
   template for P1 #8.
   - Packets are dropped on `EAGAIN` (`:345`, `:356`).
   - BL and EL are paired by arrival order, not PTS (`:376`).
   - The EL is dropped for the whole run if the BL gets 8 pictures ahead
     (`:371`).
   - The context leaks on an `avcodec_open2` failure (`:221`).
   - Allocations are unchecked.
7. **Smaller issues:**
   - `qf[qfam]` is read past its end when there's no graphics queue
     (`:387`).
   - `VK_INCOMPLETE` from enumerating devices is treated as an error
     (`:354`).
   - Shader modules leak on `pipeline()` errors (`:292-327`).
   - `memtype` falls back to type 0 instead of failing (`:136`).
   - The JSON writes file names unescaped (`dvbench.c:482`).
   - `fopen` failures for `--json` and `--dump` are silent.
   - `bundle/run-bench.sh` always exits 0.
   - `scripts/make-bundle.sh:13` hides a missing `libavcodec`.
   - `scripts/build-ffmpeg.sh` doesn't rebuild when the patches or flags
     change.
8. **Safeguards that cost nothing at run time:**
   - `_Static_assert(sizeof(struct params) == 1456)`
   - `_Static_assert(offsetof(struct params, packets) == 816)`
   - an assert that a specialisation map fits its 16 entries (rule 5 in
     `ENGINE.md`)

## P3: debt and cleanup

1. **Measurement knobs in production code.** These all need to be either
   compile-time options (`-DPI_MEASURE`) or `pi_options` fields:
   - `PI_ABLATE` and `PI_MARK`, which are in the pipeline keys
   - `PI_PIECE_GENERAL`, `PI_ALL_ROWS`, `PI_DRAW_BARS` and `PI_COPY_OUTPUT`
2. **Modes.**
   - Drop `exact32`: it is slower than `exact64` and never chosen. That
     frees `fprod`, a 30-line branch, 192 bytes of the UBO and
     `PI_MMR_EXACT32`.
   - Keep `float` as the MMR96/128 fallback only until P2 #4 is fixed.
   - Unrolling `exact64` (three nested loops, against rule 4) may speed up
     the fallback.
   - On the piecewise route, `--mmr float` turns off the fast path.
3. **A shipping path breaks shader rule 3.** `PIECE==2` uses `uint o[4]`,
   filled in a loop (`p1_chroma.frag:408-434`).
   - **Fix:** a `row422(cx, k, r)` helper, called twice with literal `r`.
   - `reshape_chroma` (`:390`) has several returns: make it one.
4. **Dead code in the engine:**
   - `sand()` in `common.glsl`
   - `SPLIT_W`
   - the UBO fields `sel` and `sel2` (written, never read)
   - descriptor binding 9 (no shader uses it)
   - `p->horizontal_fused` handling
   - `pass()`, which duplicates `pass_bands()`
   - the unused third entries of `rp[3]`, `fb[3]` and `pass_ms[3]`
   - an unreachable branch in `float_mmr` (`:635`)
   - `spec[0] = PI_MMR_FLOAT`
5. **Duplicated code:**
   - `pw_add64` and `add64`
   - the SAND word addressing, written three times
   - the guide row (`p1:426` against `guide_row`)
   - the black-pixel packing, in C and in GLSL
6. **Names and comments:**
   - `spv_p3` is now P2.
   - The enum names don't match the documented modes: `PI_MMR_HYBRID` is
     `split` and `PI_MMR_BOUNDED` is `exact`.
   - Comments still mention "three passes" and "pass 3", and leave out mode 4
     (`p1:23`).
   - `PI_MARK` writes 65535, not the documented 4095 (`p1:48`).
   - `--filters yblod` is named after a removed engine; `phase` would
     describe it.
   - A 4-way nested ternary sits at `:400`.
   - The magic numbers (`T_*`, 3072, `0x07`/4 SAND128, `0xff000000`) need
     names.
7. **`pi_submit` is 207 lines.** Split it into plan, upload tables, choose
   variants and record. All tables are re-uploaded every frame, so track
   `tables_generation` per slot.
8. **Tools.**
   - Remove `vk-micro` and `bench/micro/`: their question is answered, they
     describe yblod and CB1 as current, and they are the only reason the
     build needs EGL/GLES.
   - Cut `probe-dmabuf` down to a `dvbench --probe` built on `alloc.c`. As
     it stands it only tries udmabuf, returns 0 when it fails, and crashes on
     its error paths.
   - Remove the P010 software layout (`source.c`), which the engine
     rejects.
   - Remove `dv_drm`, `dv_is_sand128` and `dv_dmabuf_kind`, which have no
     callers.
9. **Build and scripts.**
   - Drop `meson`, `python3-jinja2`, `python3-markupsafe` and
     `libgl1-mesa-dri` from `docker/packages.txt`.
   - Drop the git branch of `scripts/fetch-sources.sh`.
   - Drop `-e DV_THREADS` (`in-docker.sh`).
   - Drop `.cache/` from `.gitignore`.
   - Fix the stale comments in `source.c`/`.h` and `vm/rpi5dv.yaml`.
10. **Test clips.**
    - `tests/make-test-clips.sh` can't reproduce the golden clip names, so
      they were renamed by hand; give it an output-name argument.
    - Its header says `SECONDS`, but it reads `SECONDS_`.
    - It doesn't create `clips/`.
    - `check.sh --validate` uses the first golden clip even when it's
      missing.
11. **The image note.** `image/customize.sh` tells users to run
    `run-bench.sh clips/*` with the default filters. Profile 8.1 MMR clips
    need `--filters yblod`, so `run-bench.sh` should pick the filters per
    clip.

## What checked out

- The C and GLSL agree on the UBO layout, the descriptor bindings and the
  specialisation constant IDs. There are no push constants.
- The `exact32` accumulators stay within range. The error in
  `fprod_split` (about 2⁻¹⁶) is far below the rounding threshold.
- The letterbox row window is 8-aligned, so the odd-row case can't happen.
- `engine/planner/README.md` and `patches/ffmpeg/README.md` match the build.
