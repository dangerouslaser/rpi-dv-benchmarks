/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Optional "CM2.9 to CM4.0" metadata and L2 trim removal for the film's DM
 * payload (dvvk_plan_in.dm_options, include/dvvk_plan.h), as CroqueMr's CB1
 * does it (CB1/src/cb1_cmv40.c, CB1/docs/CM4-APPEND.md), here on the payload
 * yblod's serializer writes (dv_ffmpeg_metadata.c: 71 header bytes, byte 70
 * the block count, each block a 4-byte big-endian length, the level and the
 * byte-aligned fields; source_max_pq at bytes 66-67) instead of FFmpeg's
 * AVDOVIMetadata. yblod/plan.c calls it before CB1's ordering (cb1_wire), so
 * the appended blocks take their place among the DM v2 blocks there.
 *
 * Adapted from CroqueMr's cb1_cmv40.c (GPL-2.0-or-later), whose notice says:
 *   Decision rules adapted from avdvplus/xbmc e0614d7a,
 *   BitstreamConverterDoVi.cpp and HDR10PlusConvert.cpp,
 *   Copyright (C) 2010-2018 Team Kodi, GPL-2.0-or-later.
 *   Neutral blocks/PQ formulas adapted from quietvoid/dovi_tool d1abe0e2, MIT.
 *   Copyright (c) 2026 quietvoid. See LICENSES/MIT-dovi-tool-cmv40.txt.
 * The MIT notice is installed with the package's licences
 * (/usr/share/licenses/dv-engine-vk/MIT-dovi-tool-cmv40.txt).
 */
#include "dvvk_plan.h"
#include <math.h>
#include <string.h>

static unsigned get16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

/* The Auto trigger's mastering brightness: CE's nominal source codes, its
 * 96-nit floor below 2055 (for this decision only), else ST 2084 rounded to
 * a whole nit (cb1_cmv40.c mastering_nits). */
static unsigned mastering_nits(unsigned pq)
{
    if (pq < 2055)
        return 96;
    switch (pq) {
    case 3079: return 1000;
    case 3388: return 2000;
    case 3696: return 4000;
    case 4095: return 10000;
    }
    double p = pow(pq / 4095.0, 1.0 / 78.84375);
    return (unsigned)lround(10000.0 * pow(fmax(p - 0.8359375, 0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125));
}

/* Auto 2's threshold: the TV's peak plus the headroom as a rounded 12-bit PQ
 * code (cb1_cmv40.c). */
static unsigned headroom_pq(unsigned tv_nits, unsigned headroom)
{
    double n = fmin(10000, tv_nits * (100 + headroom) / 100.0) / 10000;
    double p = pow(n, 0.1593017578125);
    return (unsigned)lround(4095 * pow((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p), 78.84375));
}

/* The four neutral CM4 blocks as yblod's dv_cm4_extension writes them from
 * the RPU bits dovi_tool's CmV40DmData::default_safe() (and CroqueMr) use:
 * L3 offsets 2048 x3 (raw 80 08 00 80 00), L9 source primaries index 0
 * P3-D65 (raw 00), L11 content type 1 cinema, white point 0 D65 (raw 01 00
 * 00 00), L254 DM mode 0, version index 2 (raw 00 02). In level order, as
 * CB1's ordering would leave them. No L8: trims are never made up. */
static const unsigned char neutral[] = {
    0, 0, 0, 6, 3, 0x08, 0x00, 0x08, 0x00, 0x08, 0x00,
    0, 0, 0, 1, 9, 0x00,
    0, 0, 0, 4, 11, 0x01, 0x00, 0x00, 0x00,
    0, 0, 0, 2, 254, 0x00, 0x02,
};
#define NEUTRAL_BLOCKS 4u

/* What CB1's cb1_cmv40_decide says for the payload's original blocks. */
static unsigned decide(uint32_t o, const unsigned char *dm, int has_l2, int cm4, long l1_max)
{
    static const unsigned triggers[] = {0, 1000, 2000, 4000, 10000};
    unsigned mode = o & 7u, trigger = o >> 3 & 7u, headroom = o >> 6 & 63u, tv = o >> 16;
    if (mode > DVVK_CM4_AUTO2 || trigger > 4 || headroom > 50)
        return DVVK_CM4_STATUS_INVALID;
    if (mode == DVVK_CM4_OFF)
        return DVVK_CM4_STATUS_OFF;
    if (cm4)
        return DVVK_CM4_STATUS_EXISTING;
    if (!has_l2 || mode == DVVK_CM4_ALWAYS)
        return DVVK_CM4_STATUS_APPENDED;
    if (mode == DVVK_CM4_NO_L2)
        return DVVK_CM4_STATUS_NOT_SELECTED;
    int tv_valid = tv > 0 && tv <= 10000;
    if (mode == DVVK_CM4_AUTO2) {
        if (!tv_valid)
            return DVVK_CM4_STATUS_NEEDS_TV;
        /* CE appends when L1 is absent, provided the TV peak is known. */
        if (l1_max < 0)
            return DVVK_CM4_STATUS_APPENDED;
        if (l1_max > 4095)
            return DVVK_CM4_STATUS_INVALID;
        return (unsigned)l1_max <= headroom_pq(tv, headroom) ? DVVK_CM4_STATUS_APPENDED : DVVK_CM4_STATUS_NOT_SELECTED;
    }
    if (!triggers[trigger] && !tv_valid)
        return DVVK_CM4_STATUS_NEEDS_TV;
    unsigned pq = get16(dm + 66);   /* source_max_pq: the mastering peak */
    if (pq > 4095)
        return DVVK_CM4_STATUS_INVALID;
    unsigned threshold = triggers[trigger] ? triggers[trigger] : tv;
    return mastering_nits(pq) <= threshold ? DVVK_CM4_STATUS_APPENDED : DVVK_CM4_STATUS_NOT_SELECTED;
}

/* dm: the payload (*bytes of it), changed in place. Returns dvvk_plan's
 * dm_status. Invalid options or a malformed payload: unchanged. The decision
 * uses the original L2; DVVK_DM_NO_L2 then drops L2 from the output whatever
 * it says. An append that wouldn't fit (with the 13-byte L5 CB1 or the
 * placement may still add) is rejected: no blocks added, L2 removal kept. */
uint32_t dvvk_dm_cm4(unsigned char dm[512], size_t *bytes, uint32_t options)
{
    if (!options)
        return DVVK_CM4_STATUS_OFF;
    if (*bytes < 71 || *bytes > 512)
        return DVVK_CM4_STATUS_INVALID;
    int has_l2 = 0, cm4 = 0, has_l5 = 0;
    long l1_max = -1;
    size_t at = 71;
    for (unsigned i = 0; i < dm[70]; ++i) {
        if (*bytes - at < 5)
            return DVVK_CM4_STATUS_INVALID;
        uint32_t len = (uint32_t)dm[at] << 24 | (uint32_t)dm[at + 1] << 16 | (uint32_t)dm[at + 2] << 8 | dm[at + 3];
        if (len > *bytes - at - 5)
            return DVVK_CM4_STATUS_INVALID;
        unsigned level = dm[at + 4];
        has_l2 |= level == 2;
        has_l5 |= level == 5;
        cm4 |= level == 3 || (level >= 8 && level <= 11) || level == 254;
        if (level == 1) {
            if (len < 6)
                return DVVK_CM4_STATUS_INVALID;
            l1_max = get16(dm + at + 7);   /* min, max, avg */
        }
        at += 5 + len;
    }
    if (at != *bytes)
        return DVVK_CM4_STATUS_INVALID;
    unsigned status = decide(options, dm, has_l2, cm4, l1_max);
    if (status == DVVK_CM4_STATUS_INVALID)
        return status;
    unsigned char out[512];
    memcpy(out, dm, 71);
    out[70] = 0;
    size_t used = 71;
    unsigned removed = 0;
    at = 71;
    for (unsigned i = 0; i < dm[70]; ++i) {
        uint32_t len = (uint32_t)dm[at] << 24 | (uint32_t)dm[at + 1] << 16 | (uint32_t)dm[at + 2] << 8 | dm[at + 3];
        if ((options & DVVK_DM_NO_L2) && dm[at + 4] == 2) {
            removed++;
        } else {
            memcpy(out + used, dm + at, 5 + len);
            used += 5 + len;
            out[70]++;
        }
        at += 5 + len;
    }
    if (status == DVVK_CM4_STATUS_APPENDED) {
        if (used + sizeof(neutral) + (has_l5 ? 0 : 13) > 512 || out[70] + NEUTRAL_BLOCKS + !has_l5 > 255) {
            status = DVVK_CM4_STATUS_REJECTED;
        } else {
            memcpy(out + used, neutral, sizeof(neutral));
            used += sizeof(neutral);
            out[70] += NEUTRAL_BLOCKS;
        }
    }
    if (removed || status == DVVK_CM4_STATUS_APPENDED) {
        memcpy(dm, out, used);
        *bytes = used;
    }
    return status | removed << 8;
}
