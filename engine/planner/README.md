# The planner (vendored)

The engine's CPU half, built as one static library. Per frame it reads the Dolby
Vision RPU (FFmpeg's `AVDOVIMetadata`, and the raw RPU for the CM v4.0 blocks)
and produces every table, metadata packet and pack constant the GPU passes read
(`dvvk_plan.h`).

**Origin:** yblod's Vulkan engine, [dangerouslaser/libreelec-yblod](https://github.com/dangerouslaser/libreelec-yblod)
at `00d9aa1a7241f2af00255a759ae02a9a34db699c`:

| From | Files |
|---|---|
| `yblod-engine/vk/yblod/` | `plan.c`, `dm_cm4.c` |
| `yblod-engine/vk/include/` | `dvvk_plan.h`, `dv_engine_vk.h` |
| `yblod-engine/src/` | everything else here |

**Our changes,** all from the Pi port:
- `plan.c` and `dvvk_plan.h`: the EL upscale is Annex B or bilinear; the old
  nearest option is planned as bilinear.
- `dv_engine_vk.h`: the declarations the port added.
- `dv_rpu_ext.c` (new): reads the extension blocks' original bytes from the
  raw RPU FFmpeg attaches to each frame (`AV_FRAME_DATA_DOVI_RPU_BUFFER`);
  the CM v4.0 levels (L3, L8–L11, L254) are re-encoded from them, and
  `AVDOVIDmData` doesn't keep them. `dv_ffmpeg_metadata.c` pairs them with
  FFmpeg's blocks (`dv_ffmpeg_ext_resolve`: a level's blocks in stream order;
  the static levels FFmpeg repeats for RPUs with `dm_compression`, and the
  metadata it repeats for a picture without an RPU, found again by FFmpeg's
  parse of them), and `plan.c` takes the RPU (`dvvk_plan_frame_state_rpu`).
  yblod read them from a trailer patched into FFmpeg; the result is the same
  bytes (every block of the test clips, and of copies re-encoded with
  `dm_compression`).

Taking this code over keeps the GPU output defined by yblod's arithmetic. The
Pi engine's output was verified identical to yblod's Vulkan engine, sample for
sample; `tests/golden/` keeps those results as the regression reference.

**Licences:** see `LICENSE` and `LICENSES/`.
- The C sources are GPL-2.0-or-later (yblod's GPL-3.0-or-later, relicensed
  by their author for this repository).
- `dm_cm4.c` is adapted from CroqueMr's `cb1_cmv40.c` and Kodi
  (GPL-2.0-or-later).
- yblod's `cb1_overlay.c` (LGPL-2.1, from libplacebo: GUI overlay colours)
  isn't vendored: nothing here draws an overlay.
- `dm_cm4.c` and `dv_cm4.c` carry code from quietvoid's dovi_tool (MIT,
  `LICENSES/MIT-dovi-tool*.txt`).
- `dv_rpu_ext.c` follows the RPU syntax of FFmpeg's `dovi_rpudec.c`
  (LGPL-2.1-or-later, used under the GPL as the LGPL allows).
- Explicit per-file notices take precedence.

**Build:** compiled with `DV_PREPARED_CHROMA DV_ENCODE_STEPS=256
DV_COLOUR_LUT` and `-ffp-contract=off`. It needs no FFmpeg patches: what it
reads is upstream FFmpeg's (`AV_FRAME_DATA_DOVI_METADATA`,
`AV_FRAME_DATA_DOVI_RPU_BUFFER`).
