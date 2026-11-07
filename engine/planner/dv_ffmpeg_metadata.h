#ifndef DV_FFMPEG_METADATA_H
#define DV_FFMPEG_METADATA_H
#include "dv_engine.h"
#include "single_reshape.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Private native adapter for the qualified source-colour route.
 * CMv4 requires DV_FFMPEG_RAW_EXTENSIONS and the matching private FFmpeg
 * header/trailer. Builds without that feature reject CMv4, never omit it.
 * Input is one complete AV_FRAME_DATA_DOVI_METADATA allocation. No parser
 * history or fixture files are used here. Outputs commit only on success. */
dv_status dv_settings_from_ffmpeg(const void *,size_t,unsigned,unsigned,
                                 dv_identity,dv_frame_settings *,
                                 unsigned char source_dm[512],size_t *);
/* Source-tunnel adapter also preserves space2; it does NOT authorize the
 * space0-only CPU/GPU colour-conversion pipeline to interpret that signal. */
dv_status dv_transport_settings_from_ffmpeg(const void *,size_t,unsigned,unsigned,
    dv_identity,dv_frame_settings *,unsigned char source_dm[512],size_t *);
/* Separate type: cannot accidentally enter the legacy composer backend.
 * Same source-colour/CM gates as the source-tunnel adapter, but retains
 * every residual-disabled mapping piece. Sampling is still a caller policy. */
typedef struct {
    dv_identity identity;
    dv_source_dm source;
    uint32_t active[4];
    dv_single_reshape mapping;
    uint32_t narrow_mask[3];
} dv_single_frame_settings;
dv_status dv_single_settings_from_ffmpeg(const void *,size_t,unsigned,unsigned,
    dv_identity,dv_single_frame_settings *,unsigned char source_dm[512],size_t *);
/* Explicit decoded sample depth (8 or10) must match the RPU. Residual-disabled
 * only; preserves the same reconstructed12bit source-colour/CM gates. Does not
 * prove codec/profile compatibility, GPU storage format or chroma siting.
 * The older single-layer API remains a strictly10bit wrapper. */
dv_status dv_single_settings_from_ffmpeg_for_depth(const void *,size_t,unsigned,unsigned,
    unsigned,dv_identity,dv_single_frame_settings *,unsigned char source_dm[512],size_t *);
#ifdef __cplusplus
}
#endif
#endif
