# The planner (vendored)

The engine's CPU half, built as one static library. Per frame it parses the Dolby
Vision RPU (`AVDOVIMetadata`) and produces every table, metadata packet and pack
constant the GPU passes read (`dvvk_plan.h`).

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
- Explicit per-file notices take precedence.

**Build:** compiled with `DV_FFMPEG_RAW_EXTENSIONS DV_PREPARED_CHROMA
DV_ENCODE_STEPS=256 DV_COLOUR_LUT` and `-ffp-contract=off`, against FFmpeg
headers carrying the Dolby Vision patches in `patches/ffmpeg/dolby-vision`.
