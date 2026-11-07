#ifndef DV_FFMPEG_METADATA_H
#define DV_FFMPEG_METADATA_H
#include "dv_engine.h"
#include "dv_rpu_ext.h"
#include "single_reshape.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The original bytes of the CM v4.0 extension blocks (levels 3, 8-11, 254),
 * which AVDOVIDmData doesn't keep: block[i] is the metadata's i-th block's,
 * level 0 where it has none (any other level, or no RPU to read them from). */
enum { DV_FFMPEG_EXT_MAX = 32, DV_FFMPEG_EXT_PARSED = 128 };
typedef struct {
    int count;
    dv_rpu_ext_block block[DV_FFMPEG_EXT_MAX];
} dv_ffmpeg_ext;
/* Blocks across pictures, kept with FFmpeg's parse of them and found again
 * by it: the static levels (10, 254) of the last RPU without dm_compression,
 * which FFmpeg repeats for the RPUs with it (they carry none), and every
 * block of the last RPU, which FFmpeg repeats for a picture without one
 * (no RPU buffer then). A stream starts from a zeroed cache. */
typedef struct {
    unsigned char parsed[DV_FFMPEG_EXT_PARSED];   /* the AVDOVIDmData */
    dv_rpu_ext_block raw;
} dv_ffmpeg_ext_pair;
typedef struct {
    int statics, last;
    dv_ffmpeg_ext_pair stat[DV_FFMPEG_EXT_MAX], prev[DV_FFMPEG_EXT_MAX];
} dv_ffmpeg_ext_cache;
/* Pairs one AV_FRAME_DATA_DOVI_METADATA allocation's blocks with the same
 * frame's AV_FRAME_DATA_DOVI_RPU_BUFFER (rpu NULL: none), a level's blocks
 * in stream order. Blocks it can't pair are left out; the adapters below
 * then reject a frame that needs them, never omit them. */
void dv_ffmpeg_ext_resolve(const void *metadata,size_t metadata_size,const uint8_t *rpu,size_t rpu_size,
                           dv_ffmpeg_ext_cache *cache,dv_ffmpeg_ext *out);
/* Private native adapter for the qualified source-colour route.
 * Input is one complete AV_FRAME_DATA_DOVI_METADATA allocation and its
 * dv_ffmpeg_ext (NULL: none, so no CM v4.0). Outputs commit only on success. */
dv_status dv_settings_from_ffmpeg(const void *,size_t,unsigned,unsigned,
                                 dv_identity,dv_frame_settings *,
                                 unsigned char source_dm[512],size_t *,const dv_ffmpeg_ext *);
/* Source-tunnel adapter also preserves space2; it does NOT authorize the
 * space0-only CPU/GPU colour-conversion pipeline to interpret that signal. */
dv_status dv_transport_settings_from_ffmpeg(const void *,size_t,unsigned,unsigned,
    dv_identity,dv_frame_settings *,unsigned char source_dm[512],size_t *,const dv_ffmpeg_ext *);
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
    dv_identity,dv_single_frame_settings *,unsigned char source_dm[512],size_t *,const dv_ffmpeg_ext *);
/* Explicit decoded sample depth (8 or10) must match the RPU. Residual-disabled
 * only; preserves the same reconstructed12bit source-colour/CM gates. Does not
 * prove codec/profile compatibility, GPU storage format or chroma siting.
 * The older single-layer API remains a strictly10bit wrapper. */
dv_status dv_single_settings_from_ffmpeg_for_depth(const void *,size_t,unsigned,unsigned,
    unsigned,dv_identity,dv_single_frame_settings *,unsigned char source_dm[512],size_t *,const dv_ffmpeg_ext *);
#ifdef __cplusplus
}
#endif
#endif
