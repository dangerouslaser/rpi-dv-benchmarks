/* The extension blocks of a Dolby Vision RPU, as their original bytes: what
 * the CM v4.0 levels (3, 8-11, 254) need to be re-encoded (dv_cm4_extension),
 * which FFmpeg's AVDOVIDmData doesn't keep. Read from the raw RPU NAL unit
 * FFmpeg attaches to every frame (AV_FRAME_DATA_DOVI_RPU_BUFFER).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DV_RPU_EXT_H
#define DV_RPU_EXT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { DV_RPU_EXT_MAX = 64, DV_RPU_EXT_BYTES = 32 };

typedef struct {
    uint8_t level;                /* 0: none */
    uint8_t whole;                /* the block fits bytes[]: length <= DV_RPU_EXT_BYTES */
    uint16_t length;              /* ext_block_length: the bytes after the level */
    uint8_t bytes[DV_RPU_EXT_BYTES];
} dv_rpu_ext_block;

typedef struct {
    int dm_present;               /* vdr_dm_metadata_present */
    int dm_compression;           /* the static levels are the last uncompressed RPU's */
    int count;                    /* blocks, in stream order (DM v2.9, then v4.0) */
    dv_rpu_ext_block block[DV_RPU_EXT_MAX];
} dv_rpu_ext;

/* The RPU as AV_FRAME_DATA_DOVI_RPU_BUFFER carries it: the NAL unit's
 * payload after its 2-byte header, emulation prevention included, starting
 * with the 0x19 prefix. 0 OK; 1 not an RPU of type 2 (no metadata); < 0
 * malformed or a form FFmpeg doesn't parse either. A block with
 * dm_compression of a static level (6, 10, 32, 254, 255) is listed as it
 * stands; FFmpeg ignores it. */
int dv_rpu_ext_parse(const uint8_t *rpu, size_t size, dv_rpu_ext *out);

/* FFmpeg's static levels (ff_dovi_rpu_extension_is_static). */
int dv_rpu_ext_static(unsigned level);

/* The occurrence-th block (from 0) of level in ext, or NULL. */
const dv_rpu_ext_block *dv_rpu_ext_find(const dv_rpu_ext *ext, unsigned level, unsigned occurrence);

#ifdef __cplusplus
}
#endif
#endif
