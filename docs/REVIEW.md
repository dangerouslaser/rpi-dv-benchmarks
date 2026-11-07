# Code review: before LibreELEC integration

A review of the whole repository (except the vendored planner and FFmpeg
patches), November 2026. Until then the engine had only run inside
`dvbench`, which runs once and exits. Inside Kodi it will run for hours,
through new files, seeks and stream changes. Most findings were about that
difference. Line numbers are as of commit `ff53c4e`.

Severity:
- **P0:** breaks real playback in Kodi.
- **P1:** needed for integration.
- **P2:** hardening.
- **P3:** debt and cleanup.

**Every finding below is addressed** (2026-11-07). Each is struck out, with
what was done. The changes were checked as follows:
- Every golden case reproduces in every exact MMR mode, with Vulkan
  validation clean (`tests/check.sh`, `--mmr split|exact64`, `--validate`).
- Hashes are identical to the pre-review engine on every clip, filter set and
  mode, on lavapipe and on the Pi's V3D. The one deliberate change: P3 #2's
  float fast path on route 3.
- Real-time play on the Pi (`docs/ENGINE.md` "Performance").

## P0: breaks playback in a long-lived engine

1. ~~**Rebinding an output slot renders into the old buffer**~~
   (`engine/pi_engine.c:560`). `output_half` made a new image, but it only
   made a view `if (!o->view)`, and the old objects leaked.
   - **Done:** outputs are objects the caller imports and releases
     (`pi_output_import`, `pi_output_release`). Nothing is rebound;
     releasing waits for the output's last render.
2. ~~**The pipeline caches are fixed in size, and a full one fails every
   later frame**~~ (`:112`, `:869`, `:1048`).
   - **Done:** the variant sets grow, behind a lock. A `VkPipelineCache`
     is read from and written to `pi_options.pipeline_cache`.
3. ~~**Decoder imports are never released**~~ (`:200-248`). Each import
   kept a decoder dmabuf (CMA) alive.
   - **Done:** imports belong to the stream: `pi_stream_close` releases
     them. Within a stream, imports no render has read for 64 frames, and
     none in flight reads, are released once there are more than 48 (a
     decoder that reallocates mid-stream). The import holds the buffer, so
     its inode can't be reused while it is listed.
4. ~~**The input frames' lifetime is undefined.**~~ The GPU read BL and EL
   after `pi_submit` returned.
   - **Done:** `pi_render` takes `av_frame_clone` references and drops them
     when the render's fence is seen.
5. ~~**`pi_destroy` leaks everything but the device and instance**~~
   (`:1129-1135`).
   - **Done:** `pi_engine_destroy` tears everything down in reverse order,
     and is what a failed create calls. Validation reported 214 objects
     alive at `vkDestroyDevice` before; it reports none now.
6. ~~**Profile 8.4 isn't real time on the Pi.**~~ It took 184–298 ms per
   picture: the fast piecewise path needed the split MMR, and the split's
   error check (64 × 2⁻²⁴ × m < 0.25) refused this stream.
   - The fast path runs the stream's `--mmr` mode.
   - The check is 32 × 2⁻²⁴ × m < 0.5, from a count of the roundings.
   - A frame that fails it checks each sample (`SPLIT_GUARD`).
   - **Done:** 30.8 ms, and the whole film plays with 1 late frame.
     `tests/golden/hashes.txt` has 8.4 cases (4K with both filter sets,
     1080p), and `bundle/run-bench.sh`'s `--play` is the real-time check.

## P1: what integration needs

1. ~~**An API for a long-lived caller.**~~
   - **Done:** `engine/pi_engine.h`. The calls are `pi_engine_create`
     (options), `pi_stream_open`/`close`, `pi_output_size`/`import`/`release`
     and `pi_render` (sync_files). Errors are `enum pi_status`.
     `docs/ENGINE.md` "Using the engine" summarises it.
2. ~~**Sync with the display plane.**~~
   - **Done:** `pi_render` returns a sync_file signalled when the output is
     complete: the submission's fence, exported as `SYNC_FD`. Exporting
     resets the fence, so the engine tracks that render by its own copy of
     the sync_file. (A first version signalled an exportable semaphore
     instead. On v3dv that cost the overlap of consecutive frames: 1917
     dropped 0, 5 and 1 frames in three 120 s runs, against none without
     fences. With the fence, 0, 0, 0 and 1 (0.8 ms) in four.) Without
     `VK_KHR_external_fence_fd`, the semaphore is the fallback.
   - It takes an acquire sync_file, imported as a semaphore. One already
     signalled (the usual case) is closed instead, for the same reason.
   - Queue-family ownership moves to and from `VK_QUEUE_FAMILY_FOREIGN_EXT`
     for the decoder buffers and the output. The barriers cost nothing
     measurable.
   - `dvbench --play` drives all of it as a display would. Without
     `VK_KHR_external_semaphore_fd` (lavapipe) it waits on the CPU.
   - Implicit fences alone are not enough, though they exist. v3d fences
     the output dmabuf, and KMS waits on that fence: a commit without a
     sync_file, made while a 46 ms render was still drawing, took 48 ms and
     captured the finished frame. But the fence appears only once v3dv has
     handed the job to the kernel, after `vkQueueSubmit` returns.
     `dvbench --implicit-sync` found no fence right after submit (12 of 12)
     and one 2 ms later (12 of 12). A commit in that window would show an
     unfinished frame. So Kodi gives KMS the release sync_file as
     `IN_FENCE_FD`.
   - Tested directly: `dvbench --kms-writeback` puts the output on a KMS
     plane with the sync_file as `IN_FENCE_FD`, captures it with the vc4
     writeback connector, and compares the capture with the finished frame.
     The output was pre-filled with a marker. Result: 12 of 12 captures are
     the finished frame, with no marker, both with a 4.7 ms render and with
     a 46 ms one committed while still rendering.
3. ~~**Sizes other than 3840×2160.**~~
   - **Done:** the size is the stream's (`pi_stream_open`), and outputs are
     validated against it. Dolby's 1080p Profile 5, 8.1 and 8.4 samples
     render at 4.7 ms. They match yblod's render of the same pictures,
     centred in a 4K canvas, except the metadata packets' LSBs.
   - A refusal is `PI_E_UNSUPPORTED_PLAN` (or `PI_E_BAD_RPU`), distinct from
     `PI_E_UNSUPPORTED_FORMAT`.
4. ~~**Pipelines compile on the render thread**~~ (`:875`), and P1's key
   changes with the scene's MMR order.
   - **Done:** after a stream's first frame, a worker thread at idle
     priority builds the variants the stream may need next: all MMR order
     pairs, and the split's guard on and off. A render thread that still
     misses compiles that one variant itself.
   - The pipeline cache file (P0 #2) makes later runs fast.
5. ~~**K5: planner state across streams.**~~
   - **Done:** `pi_stream_open` calls `dvvk_plan_state_reset`, and marks
     every context's tables stale. `dvbench --reopen` renders a stream,
     closes it, opens it again in the same engine and renders it again: all
     pictures identical (Power Rangers, Sol 8.4, an 8.1 MMR cut). Without
     the reset none are, the metadata packet ids running on.
6. ~~**Logging and options.**~~
   - **Done:** `struct pi_options` at create. It has a log callback with
     levels (stderr without one) and one flag per warning, so each warns
     once. The engine reads no environment; `static int shown` is gone.
     dvbench maps the old `PI_*` variables onto the options.
7. ~~**Packaging.**~~
   - **Done:** glslc is optional. Without it the build uses the committed
     SPIR-V binaries (`engine/shaders/spirv/*.spv`), and the binary is
     byte-identical. `update-spirv` refreshes them, and the build warns while
     they are stale.
   - Unoptimised SPIR-V (glslang without `-O`) runs 7–9% slower on V3D. So
     LibreELEC's glslang alone isn't a substitute for glslc, and the
     committed files are built with `-O`.
   - `option(PI_BUILD_BENCH OFF)`; bench/ alone needs libdrm.
   - `install()` rules for the libraries and `pi_engine.h`.
   - `LICENSE`: GPL-2.0-or-later, as Kodi. The planner was yblod's
     GPL-3.0-or-later; its author relicensed it here. Its third-party parts
     (dovi_tool's MIT, libplacebo's LGPL-2.1, CroqueMr's and Kodi's
     GPL-2.0-or-later in `dm_cm4.c`) all allow that. README.md and ENGINE.md
     say so, and that Kodi's FFmpeg needs `patches/ffmpeg`.
8. ~~**Input glue to move next to the engine.**~~
   - **Done:** `engine/pi_input.c` has `pi_p7_split` (the single-track
     Profile 7 split), `pi_chroma_top_left` and `pi_is_sand128` (used by the
     engine's own input check). `pi_output_size` is in the engine.
   - Pairing BL and EL pictures stays with the decoder loop (`bench/source.c`
     `take()`, the reference): see P2 #6.

## P2: hardening

1. ~~**The input frame isn't checked**~~ (`pi_engine.c:662`).
   - **Done:** the engine checks the frame format (DRM PRIME), the
     descriptor, its object and layer counts, the SAND128 modifier, each
     plane's object index and offset, and the frame's size. Any failure is
     `PI_E_UNSUPPORTED_FORMAT`.
2. ~~**Unvalidated indices and buffers.**~~
   - **Done:** there are no slot indices any more. An output's size, stride
     and byte count are checked against `pi_output_size`'s rules and the
     dmabuf's real size.
3. ~~**Shader reads past the plane at the right edge.**~~
   - **Done:**
     - `uvq` reads the last chroma column together with its left neighbour.
     - The EL luma read (`sand1`) fetches one word.
     - The `PIECE==2` guide is now `guide_row`, which reads its second word
       only when a sample is in it.
   - Every read now holds a sample that exists. Clamping each read instead
     cost 1.3 ms on 1917. The output is bit-identical.
4. ~~**MMR96 and MMR128 streams render in float, not exactly**~~
   (`pi_engine.c:970`).
   - **Done:** routing them through `exact64` would have been wrong: their
     sums don't fit 64 bits. They get emulated 128-bit instead
     (`p1_chroma.frag` `mmr_wide`), from the planner's int128 parameters, in
     every exact mode.
   - No stream tested has these kinds. `--measure force_wide` sends a 64-bit
     LUT stream down this path; it is identical on both FEL MMR clips with
     both filter sets, and has a golden case.
5. ~~**`dvbench` accepts unknown option values as the default.**~~
   - **Done:** unknown values, malformed numbers and a second file are
     errors (exit 2).
6. ~~**Decoder feed**~~ (`bench/source.c`).
   - **Done:**
     - A packet the decoder can't take yet is kept and sent again; it is
       not dropped.
     - The EL is given up only if no EL picture has ever arrived.
     - Contexts are freed on every `open_decoder` failure, and allocations
       are checked.
     - BL and EL pair by display time. An EL picture's own timestamp is
       its packet's. Where the EL's coding order differs from the packets'
       (the "All Layers" test clip), that isn't its display time, and plain
       timestamp pairing lost pictures 1 and 4 of its first six. So
       `el_display_times()` sorts the output EL pictures' packet times
       within the EL's reorder depth (the decoder's `has_b_frames`), and
       gives each picture, in output order, the earliest one left. Pictures
       the decoder discards (leading pictures after a seek, Power Rangers)
       never come out, so they take no time.
     - Pairing then recovers from a lost picture. An EL picture whose BL is
       missing is dropped; a BL picture without its EL is shown alone.
       `DVBENCH_DROP_EL=N` discards the Nth EL picture as a test. On "All
       Layers" and Power Rangers only that picture is missing, and every
       later one has the same pixels (Power Rangers' metadata packet ids
       renumber). Output-order pairing would have mispaired every picture
       after the loss.
     - Identical to before on every golden case, and on the Pi with hardware
       decode (1917, "All Layers", Power Rangers).
7. ~~**Smaller issues.**~~
   - **Done:**
     - A device with no graphics queue is an error.
     - `VK_INCOMPLETE` is accepted.
     - Shader modules are freed on every path.
     - `memtype` fails instead of falling back to type 0.
     - JSON strings are escaped.
     - `--json`/`--dump` open and write failures are reported and fail the
       run.
     - `run-bench.sh` exits 1 on any failure.
     - `make-bundle.sh` fails without the FFmpeg libraries dvbench links.
     - `build-ffmpeg.sh` rebuilds when the patches or flags change.
8. ~~**Safeguards that cost nothing at run time.**~~
   - **Done:** static asserts on `struct params`'s size and offsets, on
     `T_WORDS`, on the packet size, and on the specialisation maps fitting.

## P3: debt and cleanup

1. ~~**Measurement knobs in production code.**~~
   - **Done:** they are `pi_options.measure` bits and `pi_options.ablate`.
     dvbench sets them from the `PI_*` variables or `--measure`.
2. ~~**Modes.**~~
   - **Done:** `exact32` is gone, with `fprod`'s 10-bit form, its branch and
     192 bytes of UBO.
   - `float` stays as a user choice only. The wide kinds no longer need it.
   - Unrolling `exact64` was measured and rejected. The guarded variant went
     from 6 spills to 92; `exact64` is now only that variant's rare fallback.
   - On the piecewise route, `--mmr float` uses the fast path (a deliberate
     output change: float there was exact before).
3. ~~**A shipping path breaks shader rule 3.**~~
   - **Done:** `row422(cx, k, r)`, called with literal `r`. `reshape_chroma`
     has one return.
4. ~~**Dead code in the engine.**~~
   - **Done:** removed:
     - `sand()` and `SPLIT_W`;
     - the `sel`/`sel2` UBO fields, binding 9 and `pass()`;
     - the third entries of `rp`, `fb` and `pass_ms`;
     - `float_mmr`'s unreachable branch and `spec[0]`.
   - There was no `horizontal_fused` handling in the engine; the planner
     sets the field.
5. ~~**Duplicated code.**~~
   - **Done:**
     - One `add64` (`common.glsl`).
     - One SAND word index (`sand_index`), used by both passes.
     - The `PIECE==2` guide is `guide_row`.
     - The pixel packing is one macro shared by C and GLSL (`shaders/pack.h`).
6. ~~**Names and comments.**~~
   - **Done:**
     - `spv_p2`.
     - `PI_MMR_SPLIT` and `PI_MMR_EXACT`.
     - Comments: two passes, every mode, `PI_MARK`'s values.
     - `--filters phase` (`yblod` still accepted).
     - `mode_name()` in place of the nested ternary.
     - Named constants: `T_*`, `PI_PACKET_PIXELS`, `PI_MOD_VENDOR_BROADCOM`
       and `PI_MOD_SAND128`, and the alpha inside `PI_PACK`.
7. ~~**`pi_submit` is 207 lines.**~~
   - **Done:** `pi_render` is split into the plan, `prepare` (the tables,
     per planner generation), `bind_inputs`, `frame_params`, `keys`,
     `record` and `submit`.
   - A context copies the tables only when `tables_generation` (or the
     route) changed since its last copy.
8. ~~**Tools.**~~
   - **Done:**
     - `vk-micro`, `bench/micro/` and `probe-dmabuf` are removed;
       `dvbench --probe` replaces the latter.
     - EGL and GLES are gone from the build image, the VM and the Pi image.
     - The P010 layout, `dv_drm` and `dv_is_sand128` are removed.
       `dv_dmabuf_kind` stays, for `--probe`.
9. ~~**Build and scripts.**~~
   - **Done:**
     - The packages are dropped (nothing used them).
     - The git branch of `fetch-sources.sh` is dropped, and so are
       `DV_THREADS` and `.cache/`.
     - The comments in `source.c`/`.h` and `vm/rpi5dv.yaml` are fixed.
10. ~~**Test clips.**~~
    - **Done:** `make-test-clips.sh` has `--name` and `--kinds`, and its
      header lists how to recreate every golden name. It reads `SECONDS_`
      consistently and creates `clips/`.
    - `check.sh --validate` uses the first golden clip that is present.
11. ~~**The image note.**~~
    - **Done:** `run-bench.sh` runs each clip with CB1's filters, and reruns
      it with the phase filters when the plan is refused. The image note
      says so.

## What checked out

- The C and GLSL agree on the UBO layout, the descriptor bindings and the
  specialisation constant IDs. There are no push constants.
- The error in `fprod_split` (about 2⁻¹⁶) is far below the rounding
  threshold.
- The letterbox row window is 8-aligned, so the odd-row case can't happen.
- `engine/planner/README.md` and `patches/ffmpeg/README.md` match the build.
