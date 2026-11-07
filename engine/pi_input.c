/* The input glue a player needs around the engine (pi_engine.h): chroma
 * siting, the decoder's buffer layout, the single-track Profile 7 split and
 * the BL / EL pairing.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "pi_engine.h"
#include "pi_internal.h"

#include <libavutil/dovi_meta.h>
#include <stdlib.h>
#include <string.h>

#include "dv_nlq_metadata.h"

int pi_chroma_top_left(const AVFrame *f, int profile)
{
    return f->chroma_location == AVCHROMA_LOC_TOPLEFT || (f->chroma_location == AVCHROMA_LOC_UNSPECIFIED && profile == 7);
}

int pi_is_sand128(uint64_t m) { return (m >> 56) == PI_MOD_VENDOR_BROADCOM && (m & 0xff) == PI_MOD_SAND128; }

int pi_p7_split(const uint8_t *in, int size, int nal_size, int layer, uint8_t *out, int room)
{
    static const uint8_t start[4] = {0, 0, 0, 1};
    if (nal_size < 1 || nal_size > 4 || size < 0 || room < 0)
        return -1;
    int n = 0;
    const uint8_t *q = in, *end = in + size;
    while (end - q > nal_size) {
        uint32_t len = 0;
        for (int i = 0; i < nal_size; i++)
            len = len << 8 | q[i];
        q += nal_size;
        if (len < 2 || len > (size_t)(end - q))
            break;
        int type = (q[0] >> 1) & 63;
        if (layer && type == 63 && len > 2) {
            if (n + 4 + (int)len - 2 > room)
                return -1;
            memcpy(out + n, start, 4);
            memcpy(out + n + 4, q + 2, len - 2);
            n += 4 + (int)len - 2;
        } else if (!layer && type != 63) {
            if (n + nal_size + (int)len > room)
                return -1;
            memcpy(out + n, q - nal_size, (size_t)nal_size + len);
            n += nal_size + (int)len;
        }
        q += len;
    }
    return n;
}

int pi_frame_needs_el(const AVFrame *f)
{
    const AVFrameSideData *sd = f ? av_frame_get_side_data(f, AV_FRAME_DATA_DOVI_METADATA) : NULL;
    if (!sd || sd->size < sizeof(AVDOVIMetadata))
        return PI_E_BAD_RPU;
    AVDOVIMetadata m;
    memcpy(&m, sd->data, sizeof(m));
    if (m.header_offset > sd->size || sd->size - m.header_offset < sizeof(AVDOVIRpuDataHeader) ||
        m.mapping_offset > sd->size || sd->size - m.mapping_offset < sizeof(AVDOVIDataMapping))
        return PI_E_BAD_RPU;
    AVDOVIRpuDataHeader h;
    AVDOVIDataMapping map;
    memcpy(&h, sd->data + m.header_offset, sizeof(h));
    memcpy(&map, sd->data + m.mapping_offset, sizeof(map));
    return !dv_metadata_no_residual(&h, &map);   /* the planner's own test (plan.c: residual_enabled) */
}

#define PAIR_QUEUE 32   /* pictures held per layer */

struct pi_pairer {
    int layers, el_seen, depth;
    AVFrame *q[2][PAIR_QUEUE];
    int n[2];
    int el_timed;   /* EL pictures at the queue's front whose pts is their display time */
    struct pi_pair_stats st;
};

int pi_pairer_create(int layers, struct pi_pairer **out)
{
    if (!out || layers < 1 || layers > 2)
        return PI_E_INVALID;
    *out = calloc(1, sizeof(**out));
    if (!*out)
        return PI_E_NOMEM;
    (*out)->layers = layers;
    (*out)->depth = 2;
    return 0;
}

void pi_pairer_flush(struct pi_pairer *p)
{
    if (!p)
        return;
    for (int l = 0; l < 2; l++)
        while (p->n[l])
            av_frame_free(&p->q[l][--p->n[l]]);
    p->el_timed = 0;
}

void pi_pairer_destroy(struct pi_pairer *p)
{
    pi_pairer_flush(p);
    free(p);
}

int pi_pairer_room(const struct pi_pairer *p, int layer)
{
    return p && layer >= 0 && layer < p->layers ? PAIR_QUEUE - p->n[layer] : 0;
}

void pi_pairer_stats(const struct pi_pairer *p, struct pi_pair_stats *s)
{
    *s = p->st;
}

/* The EL pictures' display times. The decoder outputs in display order, and
 * the output pictures' packet times, within the EL's reorder depth, are their
 * display times in another order: the earliest of those not yet given out is
 * the next picture's. Pictures the decoder discarded (leading pictures after
 * a seek) never come out, so they take no time. At the end every picture is
 * given one. */
static void el_display_times(struct pi_pairer *p, int end)
{
    AVFrame **q = p->q[1];
    while (p->el_timed < p->n[1] && (end || p->n[1] - p->el_timed > p->depth)) {
        int m = p->el_timed;
        for (int i = p->el_timed + 1; i < p->n[1]; i++)
            if (q[i]->pts != AV_NOPTS_VALUE && (q[m]->pts == AV_NOPTS_VALUE || q[i]->pts < q[m]->pts))
                m = i;
        int64_t t = q[m]->pts;   /* the earliest: this picture's; it gives its own to the one it came from */
        q[m]->pts = q[p->el_timed]->pts;
        q[p->el_timed++]->pts = t;
    }
}

int pi_pairer_push(struct pi_pairer *p, int layer, AVFrame *f, int el_reorder)
{
    if (!p || !f || layer < 0 || layer >= p->layers)
        return PI_E_INVALID;
    if (layer && p->st.el_absent) {   /* a decoder given up on */
        av_frame_free(&f);
        return 0;
    }
    if (p->n[layer] >= PAIR_QUEUE)
        return 1;
    p->q[layer][p->n[layer]++] = f;
    if (layer) {
        p->el_seen = 1;
        p->depth = el_reorder < 0 ? 2 : el_reorder + 2 > PAIR_QUEUE / 2 ? PAIR_QUEUE / 2 : el_reorder + 2;
        el_display_times(p, 0);
    }
    return 0;
}

static AVFrame *take(struct pi_pairer *p, int layer)
{
    AVFrame *f = p->q[layer][0];
    if (layer && p->el_timed)
        p->el_timed--;
    memmove(p->q[layer], p->q[layer] + 1, sizeof(p->q[0][0]) * (size_t)--p->n[layer]);
    return f;
}

int pi_pairer_pop(struct pi_pairer *p, int end, AVFrame **bl, AVFrame **el)
{
    if (!p || !bl || !el)
        return PI_E_INVALID;
    *bl = *el = NULL;
    if (p->layers == 2 && !p->el_seen && !p->st.el_absent && (p->n[0] >= 8 || end))
        p->st.el_absent = 1;
    int two = p->layers == 2 && !p->st.el_absent;
    while (p->n[0]) {
        if (two && end)
            el_display_times(p, 1);
        if (two && !p->el_timed && p->n[0] < PAIR_QUEUE && !end)
            return 0;   /* wait for its EL */
        AVFrame *b, *e = NULL;
        if (two && p->el_timed) {
            int64_t a = p->q[0][0]->pts, t = p->q[1][0]->pts;
            int timed = a != AV_NOPTS_VALUE && t != AV_NOPTS_VALUE;
            if (timed && t < a) {   /* an EL picture whose BL was lost */
                e = take(p, 1);
                av_frame_free(&e);
                p->st.el_dropped++;
                continue;
            }
            b = take(p, 0);
            if (!timed || a == t)
                e = take(p, 1);
        } else
            b = take(p, 0);
        if (two && !e) {   /* a BL picture whose EL was lost */
            if (pi_frame_needs_el(b)) {
                av_frame_free(&b);
                p->st.bl_dropped++;
                continue;
            }
            p->st.bl_alone++;
        }
        *bl = b;
        *el = e;
        return 1;
    }
    return 0;
}
