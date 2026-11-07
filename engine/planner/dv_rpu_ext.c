/* See dv_rpu_ext.h. The RPU syntax walked here follows FFmpeg's
 * libavcodec/dovi_rpudec.c (ff_dovi_rpu_parse, parse_ext_blocks; LGPL-2.1-or-
 * later, used under the GPL): the same fields, in the same order and widths,
 * so the blocks found are exactly the ones FFmpeg's AVDOVIMetadata holds.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "dv_rpu_ext.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t bits, pos;
    int over;   /* read past the end */
} bits;

static uint32_t get(bits *b, unsigned n)   /* n <= 32 */
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (b->pos >= b->bits) {
            b->over = 1;
            return 0;
        }
        v = v << 1 | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u);
        b->pos++;
    }
    return v;
}

static void skip(bits *b, size_t n)
{
    if (b->pos + n > b->bits)
        b->over = 1, b->pos = b->bits;
    else
        b->pos += n;
}

static size_t left(const bits *b) { return b->bits - b->pos; }
static void align(bits *b) { skip(b, (8 - (b->pos & 7)) & 7); }

/* ue(v), up to 32 bits of value */
static uint32_t ue(bits *b)
{
    unsigned zeros = 0;
    while (!get(b, 1)) {
        if (b->over || ++zeros > 31) {
            b->over = 1;
            return 0;
        }
    }
    return (uint32_t)((1ull << zeros) - 1 + get(b, zeros));
}

static void se(bits *b) { (void)ue(b); }

/* FFmpeg's get_ue_coef / get_se_coef: the integer part, then the fraction */
static void coef(bits *b, unsigned data_type, unsigned denom, int is_signed)
{
    if (data_type == 0) {
        if (is_signed)
            se(b);
        else
            (void)ue(b);
        skip(b, denom);
    } else
        skip(b, 32);   /* float */
}

int dv_rpu_ext_static(unsigned level)
{
    return level == 6 || level == 10 || level == 32 || level == 254 || level == 255;
}

/* One extension-block section: num_ext_blocks, alignment, then each block's
 * length, level and length bytes. */
static int ext_blocks(bits *b, dv_rpu_ext *out)
{
    uint32_t n = ue(b);
    align(b);
    if (b->over || n > DV_RPU_EXT_MAX)
        return -1;
    while (n--) {
        uint32_t length = ue(b);
        unsigned level = get(b, 8);
        if (b->over || length > left(b) / 8 || out->count == DV_RPU_EXT_MAX)
            return -1;
        dv_rpu_ext_block *x = &out->block[out->count++];
        memset(x, 0, sizeof(*x));
        x->level = (uint8_t)level;
        x->length = (uint16_t)length;
        x->whole = length <= DV_RPU_EXT_BYTES;
        if (x->whole) {
            bits copy = *b;
            for (uint32_t i = 0; i < length; i++)
                x->bytes[i] = (uint8_t)get(&copy, 8);
        }
        skip(b, (size_t)length * 8);
    }
    return b->over ? -1 : 0;
}

int dv_rpu_ext_parse(const uint8_t *rpu, size_t size, dv_rpu_ext *out)
{
    memset(out, 0, sizeof(*out));
    if (!rpu || size < 6)
        return -1;
    /* the RBSP: emulation prevention removed (00 00 03 -> 00 00) */
    uint8_t stack[1024], *raw = size <= sizeof(stack) ? stack : malloc(size);
    if (!raw)
        return -1;
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        if (i >= 2 && rpu[i] == 3 && rpu[i - 1] == 0 && rpu[i - 2] == 0 && n >= 2 && raw[n - 1] == 0 && raw[n - 2] == 0)
            continue;
        raw[n++] = rpu[i];
    }
    int rc = -1;
    if (raw[0] != 25)   /* the NAL prefix */
        goto done;
    bits b = {raw + 1, (n - 1) * 8, 0, 0};

    /* rpu_data_header() */
    if (get(&b, 6) != 2) {   /* rpu_type */
        rc = 1;
        goto done;
    }
    unsigned rpu_format = get(&b, 11);
    skip(&b, 4 + 4);   /* vdr_rpu_profile, vdr_rpu_level */
    if (!get(&b, 1))   /* vdr_seq_info_present: FFmpeg doesn't parse its absence */
        goto done;
    skip(&b, 1);       /* chroma_resampling_explicit_filter_flag */
    unsigned data_type = get(&b, 2), denom = 32;
    if (data_type > 1)
        goto done;
    if (data_type == 0) {
        denom = ue(&b);
        if (denom < 13 || denom > 32)
            goto done;
    }
    skip(&b, 2 + 1);   /* vdr_rpu_normalized_idc, bl_video_full_range_flag */
    if (rpu_format & 0x700)
        goto done;
    unsigned bl_depth = ue(&b) + 8, el_depth = (ue(&b) & 0xff) + 8;
    (void)ue(&b);      /* vdr_bit_depth_minus8 */
    if (bl_depth > 16 || el_depth > 16)
        goto done;
    skip(&b, 1);       /* spatial_resampling_filter_flag */
    out->dm_compression = (int)get(&b, 3);
    skip(&b, 1);       /* el_spatial_resampling_filter_flag */
    int nlq = !get(&b, 1);   /* !disable_residual_flag */
    out->dm_present = (int)get(&b, 1);
    if (out->dm_compression > 1 || (out->dm_compression && !out->dm_present))
        goto done;
    int use_prev = (int)get(&b, 1);

    if (use_prev)
        (void)ue(&b);  /* prev_vdr_rpu_id */
    else {
        unsigned pivots[3];
        (void)ue(&b);  /* vdr_rpu_id */
        (void)ue(&b);  /* mapping_color_space */
        (void)ue(&b);  /* mapping_chroma_format_idc */
        for (int c = 0; c < 3; c++) {
            uint32_t p = ue(&b);
            if (p > 7)
                goto done;
            pivots[c] = p + 2;
            skip(&b, (size_t)pivots[c] * bl_depth);
        }
        unsigned nlq_method = 0;
        if (nlq) {
            nlq_method = get(&b, 3);
            skip(&b, 2 * (size_t)bl_depth);
            if (nlq_method != 0)
                goto done;
        }
        (void)ue(&b);  /* num_x_partitions_minus1 */
        (void)ue(&b);  /* num_y_partitions_minus1 */
        /* vdr_rpu_data_payload() */
        for (int c = 0; c < 3; c++)
            for (unsigned i = 0; i + 1 < pivots[c]; i++) {
                uint32_t idc = ue(&b);
                if (idc == 0) {   /* polynomial */
                    uint32_t order = ue(&b) + 1;
                    if (order > 2)
                        goto done;
                    if (order == 1 && get(&b, 1))   /* linear_interp_flag */
                        goto done;
                    for (uint32_t k = 0; k <= order; k++)
                        coef(&b, data_type, denom, 1);
                } else if (idc == 1) {   /* MMR */
                    unsigned order = get(&b, 2) + 1;
                    if (order > 3)
                        goto done;
                    coef(&b, data_type, denom, 1);
                    for (unsigned k = 0; k < order * 7; k++)
                        coef(&b, data_type, denom, 1);
                } else
                    goto done;
            }
        if (nlq)
            for (int c = 0; c < 3; c++) {
                skip(&b, el_depth);           /* nlq_offset */
                coef(&b, data_type, denom, 0);  /* vdr_in_max */
                coef(&b, data_type, denom, 0);  /* linear_deadzone_slope */
                coef(&b, data_type, denom, 0);  /* linear_deadzone_threshold */
            }
    }

    if (out->dm_present) {
        /* vdr_dm_data_payload() */
        (void)ue(&b);  /* affected_dm_metadata_id */
        (void)ue(&b);  /* current_dm_metadata_id */
        (void)ue(&b);  /* scene_refresh_flag */
        if (!out->dm_compression)
            skip(&b, 9 * 16 + 3 * 32 + 9 * 16 + 16 + 16 + 16 + 32 + 5 + 2 + 2 + 2 + 12 + 12 + 10);
        if (ext_blocks(&b, out))
            goto done;
        if (left(&b) > 48 && ext_blocks(&b, out))   /* DM v4.0; else padding + CRC32 + terminator */
            goto done;
    }
    rc = b.over ? -1 : 0;
done:
    if (raw != stack)
        free(raw);
    return rc;
}

const dv_rpu_ext_block *dv_rpu_ext_find(const dv_rpu_ext *ext, unsigned level, unsigned occurrence)
{
    for (int i = 0; ext && i < ext->count; i++)
        if (ext->block[i].level == level && occurrence-- == 0)
            return &ext->block[i];
    return NULL;
}
