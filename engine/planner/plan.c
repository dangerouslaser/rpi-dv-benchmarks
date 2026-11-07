/* See include/dvvk_plan.h. Mirrors yblod-engine b7c39bf + big-fella-el-scaler.patch:
 * dv_source_renderer_submit_input / present_internal (dv_source_renderer.c)
 * and reconstruct_impl / reconstruct_single_p010 / reconstruct_piecewise_surface
 * (opencl_backend.c), with native-dv.env's settings fixed: SPACE2,
 * SINGLE_PIECEWISE, SINGLE_FULL_HEIGHT, SINGLE_NV12, SINGLE_ORDER2_CHROMA,
 * RECON_ACTIVE_ROWS, PRIVATE_RESIZE, DIRECT_P010, FILTER_FUSED, LUMA_PAIR,
 * BASE_PREP_FUSED, LUMA_GUIDE_FUSED, MMR_64, MMR_96, MMR_UNIVARIATE_LUT,
 * MMR_DUAL, MMR_HORIZONTAL_FUSED (no MMR_PAIR/UNIFORM/MULHI). */
#include "dvvk_plan.h"
#include "dv_engine_vk.h"   /* its layout asserts, compiled with the engine */
#include "dv_ffmpeg_metadata.h"
#include "mmr_prepare.h"
#include "mmr64_lut.h"
#include "pixel_polynomial.h"
#include "single_reshape.h"
#include "source_geometry.h"
#include "source_overlay.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* See dvvk_plan_state in dvvk_plan.h. */
typedef struct {
    unsigned char previous[512];
    size_t size;
    unsigned long long frames;
    unsigned id;
    int was_contract;
} packet_state;

/* The composer configuration's header: the depths, the coefficient
 * denominator, the residual flag... (every field before the components). */
#define CFG_HEAD offsetof(dv_intel_composer_config, component)

/* One component's tables in plan_dual, keyed on what they are a function of:
 * the header and that component (luma, a polynomial chroma component), or
 * for an MMR chroma component's residual table only the header and its NLQ
 * (dv_mmr_prepare's residual: the EL's 1024 codes over a zero base layer, the
 * same MMR value for each). A FEL film (1917) sends new MMR coefficients
 * every picture but keeps its luma curve and NLQ for whole scenes. */
typedef struct {
    int valid;
    unsigned char head[CFG_HEAD];
    dv_component_config component;
    uint16_t mapping[1024];
    int32_t residual[1024];
} component_tables;

typedef struct {
    int valid;
    unsigned char head[CFG_HEAD];
    uint32_t nlq_offset;
    dv_exact_coefficient nlq[3];
    int32_t residual[1024];
} nlq_table;

struct dvvk_plan_state {
    packet_state packet;
    uint32_t generation;    /* bumped by every table planning and route change */
    uint32_t route;         /* the route of the last tables */
    struct {
        int valid;
        dv_intel_composer_config cfg;
        uint32_t el_scaler, chroma_phase, contract;
        dvvk_plan out;      /* plan_dual's fields */
    } dual;
    /* plan_dual's parts, for a configuration the dual entry isn't */
    component_tables luma, poly[2];
    nlq_table nlq[2];
    struct {
        int valid;
        dv_intel_composer_config cfg;
        uint16_t table[3][1024];
    } single;
    struct {
        int valid;
        dv_single_reshape mapping;
        unsigned depth;
        uint16_t luma[1024];
    } piecewise;
};

uint32_t dvvk_dm_cm4(unsigned char dm[512], size_t *bytes, uint32_t options);   /* yblod/dm_cm4.c */
int dv_reference_luma(const dv_intel_composer_config *, uint16_t, uint16_t, int32_t *, int32_t *, uint16_t *);
int dv_reference_chroma_batch(const dv_intel_composer_config *, unsigned, size_t, const uint16_t *, const uint16_t *,
                              const uint16_t *, const uint16_t *, uint16_t *, int32_t *, int32_t *, uint16_t *);

_Static_assert(sizeof(dv_single_reshape) == 4544, "GPU reshape_mapping layout");

void dvvk_plan_layout(size_t out[6])
{
    out[0] = sizeof(dvvk_plan);
    out[1] = offsetof(dvvk_plan, residual);
    out[2] = offsetof(dvvk_plan, route);
    out[3] = offsetof(dvvk_plan, mapping);
    out[4] = offsetof(dvvk_plan, packets);
    out[5] = offsetof(dvvk_plan, packet_count);
}

static uint32_t checksum(const unsigned char *data, size_t bytes)
{
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        value ^= (uint32_t)data[i] << 24;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value << 1) ^ ((value >> 31) ? UINT32_C(0x04c11db7) : 0u);
    }
    return value;
}

/* dv_source_renderer.c packetize(), unchanged. */
static unsigned packetize(const unsigned char *dm, size_t bytes, unsigned id, unsigned char out[640])
{
    unsigned count = bytes <= 119 ? 1u : 1u + (unsigned)((bytes - 119u + 120u) / 121u);
    size_t offset = 0;
    memset(out, 0, 640);
    for (unsigned i = 0; i < count; ++i) {
        unsigned char *p = out + i * 128;
        unsigned type = count == 1 ? 0 : i == 0 ? 1 : i + 1 == count ? 3 : 2;
        p[0] = (unsigned char)(type << 6);
        p[1] = (unsigned char)((id << 4) | id);
        size_t start = i ? 3 : 5, space = i ? 121 : 119, take = bytes - offset;
        if (take > space)
            take = space;
        if (!i) {
            p[3] = (unsigned char)(bytes >> 8);
            p[4] = (unsigned char)bytes;
        }
        memcpy(p + start, dm + offset, take);
        offset += take;
        uint32_t crc = checksum(p, 124);
        for (unsigned b = 0; b < 4; ++b)
            p[124 + b] = (unsigned char)(crc >> (24 - b * 8));
    }
    return count;
}

/* opencl_backend.c take_row_window(). */
static void row_window(unsigned req_first, unsigned req_last, unsigned h, uint32_t out[2])
{
    out[0] = out[1] = 0;
    unsigned first = req_first, last = req_last;
    if (last <= first || first >= h)
        return;
    first &= ~7u;
    last = last > h - (h % 8u) ? h : (last + 7u) & ~7u;
    if (last > h)
        last = h;
    if (!first && last == h)
        return;
    out[0] = first;
    out[1] = last;
}

/* The entry, if it is for component c of cfg; else invalidated. */
static int component_hit(component_tables *t, const dv_intel_composer_config *cfg, unsigned c)
{
    if (t->valid && !memcmp(t->head, cfg, CFG_HEAD) && !memcmp(&t->component, &cfg->component[c], sizeof(t->component)))
        return 1;
    t->valid = 0;
    return 0;
}

static void component_keep(component_tables *t, const dv_intel_composer_config *cfg, unsigned c,
                           const uint16_t mapping[1024], const int32_t residual[1024])
{
    memcpy(t->head, cfg, CFG_HEAD);
    t->component = cfg->component[c];
    memcpy(t->mapping, mapping, sizeof(t->mapping));
    memcpy(t->residual, residual, sizeof(t->residual));
    t->valid = 1;
}

/* plan_dual's luma: the mapping of every base layer code and the residual of
 * every EL code (dv_reference_luma). */
static int luma_tables(dvvk_plan_state *st, const dv_intel_composer_config *cfg, uint16_t mapping[1024],
                       int32_t residual_table[1024])
{
    component_tables *t = &st->luma;
    if (!component_hit(t, cfg, 0)) {
        int32_t mapped, residual;
        uint16_t unused;
        for (unsigned i = 0; i < 1024; ++i) {
            if (dv_reference_luma(cfg, (uint16_t)i, (uint16_t)cfg->component[0].nlq_offset, &mapped, &residual, &unused))
                return -1;
            mapping[i] = (uint16_t)mapped;
            if (dv_reference_luma(cfg, 0, (uint16_t)i, &mapped, &residual_table[i], &unused))
                return -1;
        }
        component_keep(t, cfg, 0, mapping, residual_table);
        return 0;
    }
    memcpy(mapping, t->mapping, sizeof(t->mapping));
    memcpy(residual_table, t->residual, sizeof(t->residual));
    return 0;
}

/* A polynomial chroma component's (dv_prepare_polynomial_component). */
static int polynomial_tables(dvvk_plan_state *st, const dv_intel_composer_config *cfg, unsigned c,
                             uint16_t mapping[1024], int32_t residual[1024])
{
    component_tables *t = &st->poly[c - 1];
    if (!component_hit(t, cfg, c)) {
        if (dv_prepare_polynomial_component(cfg, c, mapping, residual))
            return -1;
        component_keep(t, cfg, c, mapping, residual);
        return 0;
    }
    memcpy(mapping, t->mapping, sizeof(t->mapping));
    memcpy(residual, t->residual, sizeof(t->residual));
    return 0;
}

/* mmr_prepare.c's coefficient words. */
static void mmr_words(uint32_t *out, __int128 value)
{
    unsigned __int128 bits = (unsigned __int128)value;
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (uint32_t)(bits >> (32 * i));
}

static __int128 mmr_coefficient(dv_exact_coefficient c, unsigned d)
{
    return (__int128)c.integer * ((__int128)1 << d) + c.fraction;
}

/* An MMR chroma component's: dv_mmr_prepare, with its residual table from the
 * NLQ entry when that's the same. dv_mmr_prepare is then, exactly: its
 * validation (dv_reference_chroma_batch's, once: every sample is valid when
 * one is; the configuration copied with residual_enabled 0, which skips
 * the residual table and nothing else, as the entry proves the flag is 0 or
 * 1), the parameter words below (as mmr_prepare.c builds them) and that
 * residual table (a function of the header and the NLQ alone). */
static int mmr_tables(dvvk_plan_state *st, const dv_intel_composer_config *cfg, unsigned c, uint32_t params[96],
                      int32_t residual[1024])
{
    nlq_table *t = &st->nlq[c - 1];
    const dv_component_config *v = &cfg->component[c];
    if (!t->valid || memcmp(t->head, cfg, CFG_HEAD) || t->nlq_offset != v->nlq_offset ||
        memcmp(t->nlq, v->nlq, sizeof(t->nlq))) {
        t->valid = 0;
        if (dv_mmr_prepare(cfg, c, params, residual) != DV_OK)
            return -1;
        memcpy(t->head, cfg, CFG_HEAD);
        t->nlq_offset = v->nlq_offset;
        memcpy(t->nlq, v->nlq, sizeof(t->nlq));
        memcpy(t->residual, residual, sizeof(t->residual));
        t->valid = 1;
        return 0;
    }
    if (v->mapping != 1)
        return -1;
    dv_intel_composer_config check = *cfg;
    check.residual_enabled = 0;
    const uint16_t zero = 0;
    uint16_t mapped, code;
    int32_t r, sum;
    if (dv_reference_chroma_batch(&check, c, 1, &zero, &zero, &zero, &zero, &mapped, &r, &sum, &code))
        return -1;
    uint32_t p[96] = {0};
    unsigned d = cfg->coefficient_log2_denominator;
    p[0] = v->mmr_order;
    p[1] = d;
    for (unsigned k = 0; k < 3; ++k) {
        p[2 + 2 * k] = cfg->component[k].pivots[0];
        p[3 + 2 * k] = cfg->component[k].pivots[cfg->component[k].pivot_count - 1];
    }
    mmr_words(p + 8, mmr_coefficient(v->mmr_constant, d) * ((__int128)1 << 20));
    for (unsigned order = 0; order < v->mmr_order; ++order)
        for (unsigned k = 0; k < 7; ++k)
            mmr_words(p + 12 + 4 * (order * 7 + k), mmr_coefficient(v->mmr[order][k], d));
    memcpy(params, p, sizeof(p));
    memcpy(residual, t->residual, sizeof(t->residual));
    return 0;
}

/* reconstruct_impl's table preparation and kernel choice, direct route. */
static int plan_dual(dvvk_plan_state *st, const dv_frame_settings *s, int contract, dvvk_plan *o)
{
    const dv_intel_composer_config *cfg = &s->composer;
    if (cfg->el_spatial_resampling_flag > 1 || s->el_scaler > 1 || s->chroma_phase > 1)
        return DV_UNSUPPORTED;
    o->equal = !cfg->el_spatial_resampling_flag;
    o->el_scale = o->equal ? 1 : 2;
    o->nearest = !s->el_scaler;
    /* 2: none. CB1 reshapes the decoded chroma as it is and enlarges the EL's 64/192. */
    o->chroma_phase = contract ? 2u : s->chroma_phase;
    if (luma_tables(st, cfg, o->mapping, o->residual[0]))
        return DV_UNSUPPORTED;
    for (unsigned c = 1; c < 3; ++c) {
        if (!cfg->component[c].mapping) {
            if (polynomial_tables(st, cfg, c, o->chroma_mapping[c - 1], o->residual[c]))
                return DV_UNSUPPORTED;
            memset(o->mmr_params[c - 1], 0, sizeof(o->mmr_params[c - 1]));
        } else if (mmr_tables(st, cfg, c, o->mmr_params[c - 1], o->residual[c]))
            return DV_UNSUPPORTED;
    }
    int lut[2] = {0, 0};
    for (unsigned c = 0; c < 2; ++c)
        if (cfg->component[c + 1].mapping && dv_mmr64_eligible(o->mmr_params[c], NULL)) {
            if (!dv_mmr64_build_lut(o->mmr_params[c], o->mmr_lut[c]))
                return DV_UNSUPPORTED;
            lut[c] = 1;
        }
    o->dual = cfg->component[1].mapping && cfg->component[2].mapping && lut[0] && lut[1] &&
              !memcmp(o->mmr_params[0] + 2, o->mmr_params[1] + 2, 6 * sizeof(uint32_t));
    o->horizontal_fused = !o->equal && o->dual;
    for (unsigned c = 0; c < 2; ++c) {
        const uint32_t *p = o->mmr_params[c];
        if (!cfg->component[c + 1].mapping)
            o->chroma_kind[c] = DVVK_CHROMA_POLY;
        else if (lut[c])
            o->chroma_kind[c] = DVVK_CHROMA_MMR64_LUT;
        else {
            /* mmr96_eligible() from opencl_backend.c */
            int e = !(p[0] < 1 || p[0] > 3 || p[1] < 15 || p[1] > 32);
            for (unsigned k = 0; e && k < 3; k++)
                if (p[2 + 2 * k] > p[3 + 2 * k] || p[3 + 2 * k] > 1023)
                    e = 0;
            if (e) {
                uint32_t sign = p[10] & 0x80000u ? UINT32_MAX : 0;
                if ((p[8] & 0xfffffu) || p[11] != sign || (p[10] & 0xfff00000u) != (sign & 0xfff00000u))
                    e = 0;
                for (unsigned k = 0; e && k < 7 * p[0]; k++) {
                    const uint32_t *v = p + 12 + 4 * k;
                    sign = v[1] & 0x80000000u ? UINT32_MAX : 0;
                    if (v[2] != sign || v[3] != sign)
                        e = 0;
                }
            }
            o->chroma_kind[c] = e ? DVVK_CHROMA_MMR96 : DVVK_CHROMA_MMR128;
        }
    }
    return DV_OK;
}

/* plan_dual, or its last result for the same composer configuration and
 * settings: the same function of the same inputs. */
static int plan_dual_cached(dvvk_plan_state *st, const dv_frame_settings *s, int contract, dvvk_plan *o)
{
    typeof(st->dual) *c = &st->dual;
#define DUAL_FIELDS(F) F(equal) F(el_scale) F(nearest) F(chroma_phase) F(mapping) F(residual) \
    F(chroma_mapping) F(mmr_params) F(mmr_lut) F(dual) F(horizontal_fused) F(chroma_kind)
#define COPY(f) memcpy(&dst->f, &src->f, sizeof(dst->f));
    if (c->valid && c->el_scaler == s->el_scaler && c->chroma_phase == s->chroma_phase &&
        c->contract == (uint32_t)contract && !memcmp(&c->cfg, &s->composer, sizeof(c->cfg))) {
        const dvvk_plan *src = &c->out;
        dvvk_plan *dst = o;
        DUAL_FIELDS(COPY)
        return DV_OK;
    }
    c->valid = 0;
    st->generation++;
    int status = plan_dual(st, s, contract, o);
    if (status == DV_OK) {
        const dvvk_plan *src = o;
        dvvk_plan *dst = &c->out;
        DUAL_FIELDS(COPY)
        c->cfg = s->composer;
        c->el_scaler = s->el_scaler;
        c->chroma_phase = s->chroma_phase;
        c->contract = (uint32_t)contract;
        c->valid = 1;
    }
#undef COPY
#undef DUAL_FIELDS
    return status;
}

static int single_tables_cached(dvvk_plan_state *st, const dv_intel_composer_config *cfg, uint16_t out[3][1024])
{
    typeof(st->single) *c = &st->single;
    if (c->valid && !memcmp(&c->cfg, cfg, sizeof(*cfg))) {
        memcpy(out, c->table, sizeof(c->table));
        return 0;
    }
    c->valid = 0;
    st->generation++;
    if (dv_prepare_polynomial_tables(cfg, out))
        return -1;
    memcpy(c->table, out, sizeof(c->table));
    c->cfg = *cfg;
    c->valid = 1;
    return 0;
}

static int luma_table_cached(dvvk_plan_state *st, const dv_single_reshape *mapping, unsigned depth, uint16_t out[1024])
{
    typeof(st->piecewise) *c = &st->piecewise;
    if (c->valid && c->depth == depth && !memcmp(&c->mapping, mapping, sizeof(*mapping))) {
        memcpy(out, c->luma, sizeof(c->luma));
        return 0;
    }
    c->valid = 0;
    st->generation++;
    if (dv_single_reshape_luma_table_for_depth(mapping, depth, out, (size_t)1 << depth))
        return -1;
    memcpy(c->luma, out, sizeof(c->luma));
    c->mapping = *mapping;
    c->depth = depth;
    c->valid = 1;
    return 0;
}

/* CB1's metadata serializer (docs/history/SYNC-CB1-2026-10-30.md; the LibreELEC port's
 * 725da4749b did this in yblod's host C for every picture, here only in the
 * contract): extension blocks by level with the DM v2 blocks (L3, L8-L11,
 * L254) after the others, ties in stream order (dvbridge_metadata.h
 * cb1_wire_block), and an L5 always (the placement fills in the raster's
 * margins). The source payload's blocks are yblod's conversions of the
 * stream's, one each in stream order (L6 dropped), so ordering them here is
 * ordering the stream's. */
static int cb1_wire(unsigned char dm[512], size_t *bytes)
{
    if (*bytes < 71 || *bytes > 512)
        return -1;
    unsigned n = dm[70], start[256], size[256], rank[256], order[256];
    size_t at = 71;
    int area = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (*bytes - at < 5)
            return -1;
        uint32_t len = (uint32_t)dm[at] << 24 | (uint32_t)dm[at + 1] << 16 | (uint32_t)dm[at + 2] << 8 | dm[at + 3];
        if (len > *bytes - at - 5)
            return -1;
        unsigned level = dm[at + 4], r = level + ((level == 3 || (level >= 8 && level <= 11) || level == 254) ? 256u : 0u);
        area |= level == 5;
        start[i] = (unsigned)at;
        size[i] = 5 + len;
        unsigned k = i;
        while (k > 0 && rank[k - 1] > r) {
            rank[k] = rank[k - 1];
            order[k] = order[k - 1];
            --k;
        }
        rank[k] = r;
        order[k] = i;
        at += 5 + len;
    }
    if (at != *bytes)
        return -1;
    unsigned char out[512];
    memcpy(out, dm, 71);
    size_t used = 71;
    for (unsigned k = 0; k < n; ++k) {
        memcpy(out + used, dm + start[order[k]], size[order[k]]);
        used += size[order[k]];
    }
    if (!area) {   /* zero margins: dv_source_place_metadata sets the raster's */
        if (used > 512 - 13 || n == 255)
            return -1;
        memset(out + used, 0, 13);
        out[used + 3] = 8;
        out[used + 4] = 5;
        used += 13;
        ++out[70];
    }
    memcpy(dm, out, used);
    *bytes = used;
    return 0;
}

dvvk_plan_state *dvvk_plan_state_new(void)
{
    return calloc(1, sizeof(dvvk_plan_state));
}

void dvvk_plan_state_reset(dvvk_plan_state *s)
{
    memset(s, 0, sizeof(*s));
}

void dvvk_plan_state_free(dvvk_plan_state *s)
{
    free(s);
}

/* The packet id. CB1's (the contract): advanced only when the payload
 * differs from the previous picture's (dv_packet_sequence_next in the
 * LibreELEC port's source_geometry.c; dvbridge_dv_metadata_output). Otherwise
 * the player's. Coming into the contract from the player's ids (a boxed film
 * back full screen) counts as a change: the next id after the last one sent.
 * The first picture after a reset: 0, as CB1's. */
static unsigned packet_id(packet_state *s, int contract, unsigned player, const unsigned char *dm, size_t bytes)
{
    if (!contract) {
        s->id = player;
        s->was_contract = 0;
        s->frames++;
        return s->id;
    }
    if (s->frames && (!s->was_contract || s->size != bytes || memcmp(s->previous, dm, bytes)))
        s->id = (s->id + 1) & 15u;
    s->frames++;
    memcpy(s->previous, dm, bytes);
    s->size = bytes;
    s->was_contract = 1;
    return s->id;
}

static unsigned get16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }

/* PQ code (12-bit) of a luminance in cd/m2 */
static unsigned pq_code(double nits)
{
    const double m1 = 2610.0 / 16384, m2 = 2523.0 / 32, c1 = 3424.0 / 4096, c2 = 2413.0 / 128, c3 = 2392.0 / 128;
    double y = pow(fmin(fmax(nits / 10000.0, 0.0), 1.0), m1);
    long code = lround(pow((c1 + c2 * y) / (1 + c3 * y), m2) * 4095.0);
    return code < 0 ? 0 : code > 4095 ? 4095 : (unsigned)code;
}

/* Desktop mode (docs/research/media-following-colour.md 3.4): the film is a
 * small part of a frame the desktop fills, so the frame gets static graphics
 * display management instead of the film's per-scene metadata (the UI's white
 * mustn't move with the film's scenes): L1 from d (min PQ(0.005), max the
 * film's peak but at least the UI white, avg fixed), no trims (L2, L3, L4, L8,
 * L10 dropped); L5, L9, L11 and L254 kept. The composer is untouched. */
static int desktop_metadata(unsigned char dm[512], size_t *bytes, const dvvk_plan_desktop *d, double white_nits)
{
    if (*bytes < 71 || *bytes > 512)
        return -1;
    unsigned char out[512];
    memcpy(out, dm, 71);
    out[70] = 0;
    size_t at = 71, used = 71;
    unsigned min = d->min_pq, max = d->max_pq, avg = d->avg_pq;
    unsigned white = pq_code(white_nits > 0 ? white_nits : 203.0);
    if (!min)
        min = pq_code(0.005);
    if (!max) {
        unsigned peak = get16(dm + 66);   /* source_max_pq: the mastering peak */
        max = peak > white ? peak : white;
    }
    if (!avg)
        avg = (min + white) / 2;
    if (min > avg || avg > max || max > 4095)
        return -1;
    /* L1 first */
    out[used + 0] = out[used + 1] = out[used + 2] = 0;
    out[used + 3] = 6;
    out[used + 4] = 1;
    put16(out + used + 5, min);
    put16(out + used + 7, max);
    put16(out + used + 9, avg);
    used += 11;
    out[70]++;
    for (unsigned i = 0; i < dm[70]; ++i) {
        if (at > *bytes || *bytes - at < 5)
            return -1;
        uint32_t n = (uint32_t)dm[at] << 24 | (uint32_t)dm[at + 1] << 16 | (uint32_t)dm[at + 2] << 8 | dm[at + 3];
        if (n > *bytes - at - 5)
            return -1;
        unsigned level = dm[at + 4];
        if (level == 5 || level == 9 || level == 11 || level == 254) {
            if (used + 5 + n > sizeof(out))
                return -1;
            memcpy(out + used, dm + at, 5 + n);
            used += 5 + n;
            out[70]++;
        }
        at += 5 + n;
    }
    memcpy(dm, out, used);
    *bytes = used;
    return 0;
}

int dvvk_plan_frame(const dvvk_plan_in *in, const void *metadata, size_t metadata_size, dvvk_plan *o)
{
    return dvvk_plan_frame_ex(in, NULL, metadata, metadata_size, o);
}

int dvvk_plan_frame_ex(const dvvk_plan_in *in, const dvvk_plan_desktop *desktop, const void *metadata,
                       size_t metadata_size, dvvk_plan *o)
{
    static dvvk_plan_state *process;
    if (!process && !(process = dvvk_plan_state_new()))
        return DV_INVALID;
    return dvvk_plan_frame_state(in, desktop, process, metadata, metadata_size, o);
}

int dvvk_plan_frame_state(const dvvk_plan_in *in, const dvvk_plan_desktop *desktop, dvvk_plan_state *state,
                          const void *metadata, size_t metadata_size, dvvk_plan *o)
{
    if (!in || !o || !state || (!metadata && metadata_size))
        return DV_INVALID;
    memset(o, 0, sizeof(*o));
    dv_source_geometry g = {
        .width = in->width, .height = in->height, .canvas_width = 3840, .canvas_height = 2160,
        .x = in->x, .y = in->y,
        .chroma_siting = (in->cb1_contract || !in->has_el) && in->chroma_top_left ? DV_SOURCE_CHROMA_TOP_LEFT
                                                                                  : DV_SOURCE_CHROMA_CENTER_LEFT,
        .destination_width = in->dw, .destination_height = in->dh,
    };
    unsigned w = in->width, h = in->height, depth = in->nv12 ? 8 : 10;
    /* DV_SINGLE_FULL_HEIGHT; CB1 reshapes at 4:2:0 and resamples the result */
    int contract = in->cb1_contract != 0, nv12 = in->nv12 != 0, full = !nv12 && !contract;
    if (!dv_source_geometry_valid(&g))
        return DV_INVALID;
    unsigned dw = dv_source_destination_width(&g), dh = dv_source_destination_height(&g);
    int resized = dw != w || dh != h;
    dv_frame_settings settings;
    dv_single_frame_settings general;
    memset(&general, 0, sizeof(general));
    unsigned char dm[512];
    size_t bytes = 0;
    dv_identity id = {0, in->pts};
    int status = dv_transport_settings_from_ffmpeg(metadata, metadata_size, w, h, id, &settings, dm, &bytes);
    int single = !in->has_el, use_piecewise = 0;
    if (status == DV_OK) {
        /* The EL upscale: 1 Annex B, else (2, and 0 which was nearest) bilinear.
         * To yblod's host code (tables, routes, siting) bilinear is Annex B;
         * only the GPU taps differ (o->nearest below). */
        settings.el_scaler = 1;
        if (!single && !in->fel && settings.composer.residual_enabled) {
            dv_intel_composer_config *cfg = &settings.composer;
            cfg->residual_enabled = 0;
            for (unsigned c = 0; c < 3; ++c) {
                dv_component_config *v = &cfg->component[c];
                memset(v, 0, sizeof(*v));
                v->pivot_count = 2;
                v->pivots[1] = 1023;
                v->polynomial_order[0] = 1;
                v->polynomial[0][1].integer = 1;
            }
            single = 1;
        }
    }
    int polynomial = status == DV_OK;
    if (polynomial)
        for (unsigned c = 0; c < 3; ++c)
            if (settings.composer.component[c].mapping)
                polynomial = 0;
    if (polynomial && !settings.composer.residual_enabled)
        single = 1;
    if ((single || status == DV_UNSUPPORTED) && !polynomial) {
        if (!full && !contract && g.chroma_siting != DV_SOURCE_CHROMA_CENTER_LEFT)
            return DV_UNSUPPORTED;
        if (full && resized)
            return DV_UNSUPPORTED;
        status = dv_single_settings_from_ffmpeg_for_depth(metadata, metadata_size, w, h, depth, id, &general, dm, &bytes);
        if (status)
            return status;
        single = 1;
        uint32_t masks[3];
        if (dv_single_reshape_surface_masks(&general.mapping, depth, masks))
            return DV_UNSUPPORTED;
        memset(&settings, 0, sizeof(settings));
        settings.abi_version = DV_ENGINE_ABI;
        settings.identity = general.identity;
        settings.source = general.source;
        memcpy(settings.active, general.active, sizeof(settings.active));
        use_piecewise = 1;
        o->layout = full ? 1u : 0u;
        o->sampling = full ? g.chroma_siting : 0u;
    }
    if (status)
        return status;
    if (nv12 && !use_piecewise)
        return DV_UNSUPPORTED;
    if (single && settings.composer.residual_enabled)
        return DV_UNSUPPORTED;
    if (!single && !contract && g.chroma_siting != DV_SOURCE_CHROMA_CENTER_LEFT)
        return DV_UNSUPPORTED;
    /* CM4 and L2 options (yblod/dm_cm4.c), on the film's payload as yblod
     * wrote it: before CB1's ordering, so the blocks added go where CB1 puts
     * the DM v2 blocks. Not on desktop frames (static display management). */
    if (!(desktop && desktop->enabled))
        o->dm_status = dvvk_dm_cm4(dm, &bytes, in->dm_options);
    if (contract && cb1_wire(dm, &bytes))
        return DV_UNSUPPORTED;
    if (dv_source_place_metadata(&g, dm, &bytes))
        return DV_UNSUPPORTED;
    for (unsigned c = 0; c < 3; ++c)
        if ((settings.source.offset[c] & 65535u) || (settings.source.offset[c] >> 16) > 4095)
            return DV_UNSUPPORTED;

    /* DV_RECON_ACTIVE_ROWS (not for NV12) */
    if (!nv12 && settings.active[1] < settings.active[3]) {
        unsigned margin = 8 + 4 * ((h + dh - 1) / dh);
        unsigned first = settings.active[1] > margin ? settings.active[1] - margin : 0, last = settings.active[3] + margin;
        row_window(first, last < h ? last : h, h, &o->row_first);
    }

    o->width = w;
    o->height = h;
    o->depth = depth;
    if (use_piecewise) {
        o->route = DVVK_ROUTE_PIECEWISE;
        if (o->layout > 1 || (depth == 8 && o->layout))
            return DV_UNSUPPORTED;
        uint32_t mask[3];
        if (dv_single_reshape_surface_masks(&general.mapping, depth, mask) ||
            memcmp(mask, general.narrow_mask, sizeof(mask)))
            return DV_UNSUPPORTED;
        const dv_source_dm *sd = &general.source;
        if (sd->compressed || sd->depth != 12 || (sd->space != 0 && sd->space != 2) || sd->eotf != 65535 ||
            sd->full_range != 1 || general.active[0] >= general.active[2] || general.active[1] >= general.active[3] ||
            general.active[2] > w || general.active[3] > h)
            return DV_UNSUPPORTED;
        memcpy(o->narrow_mask, mask, sizeof(mask));
        o->order2 = o->layout == 1 && depth == 10 && dv_single_reshape_order2_chroma(&general.mapping);
        if (luma_table_cached(state, &general.mapping, depth, o->luma_table))
            return DV_UNSUPPORTED;
        memcpy(o->reshape, &general.mapping, sizeof(general.mapping));
        if (nv12)
            o->row_first = o->row_last = 0;
    } else if (single) {
        o->route = DVVK_ROUTE_SINGLE;
        if (settings.chroma_phase || settings.composer.residual_enabled)
            return DV_UNSUPPORTED;
        if (single_tables_cached(state, &settings.composer, o->single_table))
            return DV_UNSUPPORTED;
    } else {
        o->route = DVVK_ROUTE_DUAL;
        if ((status = plan_dual_cached(state, &settings, contract, o)) != DV_OK)
            return status;
        /* The shaders' EL filter: 0 Annex B, 2 bilinear (nearest requests too). */
        o->nearest = in->el_scaler == 1 ? 0 : 2;
    }
    /* The tables are a function of the route and its cache entry: the same
     * generation, the same bytes (every other table is zero). */
    if (state->route != o->route) {
        state->route = o->route;
        state->generation++;
    }
    o->tables_generation = state->generation;
    /* source_identity()'s gate for the tunnel output */
    if (!use_piecewise) {
        const dv_source_dm *sd = &settings.source;
        if (sd->compressed || sd->depth != 12 || (sd->space != 0 && sd->space != 2) || sd->eotf != 65535 ||
            settings.active[0] >= settings.active[2] || settings.active[1] >= settings.active[3] ||
            settings.active[2] > w || settings.active[3] > h)
            return DV_UNSUPPORTED;
    }

    /* present_internal() */
    if (in->packet_id > 15)
        return DV_INVALID;
    if (desktop && desktop->enabled && desktop_metadata(dm, &bytes, desktop, in->white_nits))
        return DV_INVALID;
    if (in->refresh)
        dm[1] = 1;
    if (dv_source_overlay_metadata(dm, bytes, in->overlay != 0, in->overlay_visible_before))
        return DV_INVALID;
    /* CB1: the id advances only when the payload changes (plan_in.packet_id unused) */
    unsigned packet = packet_id(&state->packet, contract, in->packet_id, dm, bytes);
    o->packet_count = packetize(dm, bytes, packet, o->packets);
    int full_pack = use_piecewise && o->layout == 1;
    if (full_pack && resized)
        return DV_UNSUPPORTED;
    o->pack = full_pack ? DVVK_PACK_422 : resized ? DVVK_PACK_RESIZED : DVVK_PACK_SITED;
    if (contract && o->pack != DVVK_PACK_SITED)
        return DV_UNSUPPORTED;   /* CB1 doesn't call resized Master output exact */
    o->chroma_filter = contract ? 1u : 0u;
    if (dv_source_map_active(&g, dw, dh, settings.active, o->active))
        return DV_INVALID;
    for (unsigned c = 0; c < 3; ++c)
        o->black[c] = settings.source.offset[c] >> 16;
    o->placement[0] = g.x;
    o->placement[1] = g.y;
    o->placement[2] = dw;
    o->placement[3] = dh;
    o->top_left = g.chroma_siting;
    o->source_w = w;
    o->source_h = h;
    o->overlay_visible = in->overlay != 0;
    if (in->overlay) {
        /* prepare_overlay() in dv_source_renderer.c */
        dv_overlay_colour colour;
        if (dv_overlay_colour_init(&settings.source, in->rgb_to_lms, in->white_nits, &colour))
            return DV_UNSUPPORTED;
        float *coeff = o->gui_coeff;
        for (unsigned i = 0; i < 9; ++i) {
            coeff[i] = (float)colour.source.inverse_ycc[i];
            coeff[i + 9] = (float)colour.source.inverse_lms[i];
            coeff[i + 18] = (float)colour.rgb_to_lms[i];
        }
        for (unsigned i = 0; i < 3; ++i)
            coeff[i + 27] = (float)colour.source.offset[i];
        coeff[30] = (float)colour.white_nits;
        for (unsigned i = 0; i < 31; ++i)
            if (!isfinite(coeff[i]))
                return DV_UNSUPPORTED;
    }
    return DV_OK;
}

/* For the crate's unit tests (src/plan.rs): plan.c's own helpers, as they
 * are. Not in the header and not exported by the library. */
unsigned dvvk_test_packetize(const unsigned char *dm, size_t bytes, unsigned id, unsigned char out[640])
{
    return packetize(dm, bytes, id, out);
}

int dvvk_test_cb1_wire(unsigned char dm[512], size_t *bytes)
{
    return cb1_wire(dm, bytes);
}

unsigned dvvk_test_packet_id(dvvk_plan_state *state, int contract, unsigned player, const unsigned char *dm, size_t bytes)
{
    return packet_id(&state->packet, contract, player, dm, bytes);
}

void dvvk_test_row_window(unsigned first, unsigned last, unsigned h, uint32_t out[2])
{
    row_window(first, last, h, out);
}

uint32_t dvvk_test_dm_cm4(unsigned char dm[512], size_t *bytes, uint32_t options)
{
    return dvvk_dm_cm4(dm, bytes, options);
}
