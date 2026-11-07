/* The CPU half of a frame, done by yblod's own host code exactly as
 * dv_source_renderer.c and opencl_backend.c do it with the production
 * settings (native-dv.env): the route, every table the kernels read, the
 * letterbox row window, the placed metadata packets and the pack's constants.
 * The one copy of this header: yblod/plan.c, the engine's C API
 * (dv_engine_vk.h) and the tools include it. Rust (src/plan.rs) mirrors
 * these structs; the static asserts at the end and src/abi.rs pin the
 * layouts to the same numbers, and dvvk_plan_layout() checks at run time. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    DVVK_ROUTE_DUAL = 1,        /* FEL: BL + EL, residual (opencl reconstruct_p010) */
    DVVK_ROUTE_SINGLE = 2,      /* polynomial, no residual (single_polynomial_p010) */
    DVVK_ROUTE_PIECEWISE = 3,   /* general mapping (single_reshape.cl), P010 or NV12 */
};
enum { DVVK_CHROMA_POLY = 0, DVVK_CHROMA_MMR128 = 1, DVVK_CHROMA_MMR96 = 2, DVVK_CHROMA_MMR64_LUT = 3 };
enum { DVVK_PACK_SITED = 0, DVVK_PACK_422 = 1, DVVK_PACK_RESIZED = 2 };

typedef struct {
    uint32_t width, height;             /* decoded */
    uint32_t x, y, dw, dh;              /* destination in the canvas; dw/dh 0: no resize */
    uint32_t chroma_top_left, has_el, nv12;
    uint32_t el_scaler, fel;            /* 1 Annex B / 2 bilinear (0, formerly nearest: bilinear); 0 FEL off */
    uint32_t packet_id, refresh, overlay, overlay_visible_before;
    int64_t pts;
    double rgb_to_lms[9];               /* overlay: linear GUI RGB -> the source's LMS */
    double white_nits;                  /* overlay white (mpv-yblod: 203) */
    uint32_t cb1_contract;              /* CB1 Master Reference's spatial contract: the
                                         * stream's siting (also with an EL), 4:2:0
                                         * reshaping, no chroma phase pre-filter and the
                                         * Annex B 4:2:2 output filters */
    uint32_t dm_options;                /* the film's display-management payload: CM4
                                         * and L2 options (DVVK_DM_*, below); 0: as the
                                         * stream has it (reserved_in until 0.1.0-17) */
} dvvk_plan_in;

/* dvvk_plan_in.dm_options (dv-engine-vk 0.1.0-18, ABI 2 still: the field was
 * reserved and 0): CroqueMr's optional "CM2.9 to CM4.0" metadata (CB1's
 * cb1_cmv40.c, docs/vulkan-engine.md "CM4 metadata"), applied to the film's
 * DM payload as yblod writes it, before CB1's ordering and packetisation.
 * Metadata only: no pixel, route or mode changes. Not for desktop frames.
 *   bits 0-2   mode: DVVK_CM4_OFF, _NO_L2 (append when the stream has no L2),
 *              _ALWAYS (unless already CM4), _AUTO (no L2, or the mastering
 *              peak within the trigger), _AUTO2 (no L2, or L1 max within the
 *              TV peak plus the headroom)
 *   bits 3-5   Auto's trigger: 0 the TV peak, 1-4 1000/2000/4000/10000 nits
 *   bits 6-11  Auto 2's headroom, 0..50 (percent)
 *   bit 12     DVVK_DM_NO_L2: L2 blocks left out of the output (the CM4
 *              decision still sees them)
 *   bits 16-31 the TV's peak in nits, 0 unknown (valid 1..10000)
 * A mode, trigger or headroom out of range: status INVALID, the payload as is. */
enum { DVVK_CM4_OFF = 0, DVVK_CM4_NO_L2 = 1, DVVK_CM4_ALWAYS = 2, DVVK_CM4_AUTO = 3, DVVK_CM4_AUTO2 = 4 };
#define DVVK_DM_NO_L2 (1u << 12)
#define DVVK_DM_OPTIONS(mode, trigger, headroom, no_l2, tv_peak_nits)                              \
    ((uint32_t)((mode) & 7u) | (uint32_t)((trigger) & 7u) << 3 | (uint32_t)((headroom) & 63u) << 6 | \
     ((no_l2) ? DVVK_DM_NO_L2 : 0u) | (uint32_t)((tv_peak_nits) & 0xffffu) << 16)
/* dvvk_plan.dm_status: the CM4 result (CB1's codes) in bits 0-7, the number
 * of L2 blocks removed in bits 8-15. */
enum {
    DVVK_CM4_STATUS_OFF = 0,          /* mode off (or a desktop frame) */
    DVVK_CM4_STATUS_EXISTING = 1,     /* the stream is CM4 already (L3, L8-L11 or L254) */
    DVVK_CM4_STATUS_NOT_SELECTED = 2, /* the mode's rule says no */
    DVVK_CM4_STATUS_NEEDS_TV = 3,     /* the rule needs the TV's peak, unknown */
    DVVK_CM4_STATUS_APPENDED = 4,     /* L3, L9, L11 and L254 appended */
    DVVK_CM4_STATUS_INVALID = 5,      /* options or payload invalid: as is */
    DVVK_CM4_STATUS_REJECTED = 6,     /* wouldn't fit the payload: not appended */
};

typedef struct {
    /* 8-byte members first: no padding differences with Rust. */
    int64_t mmr_lut[2][3][1024];        /* univariate MMR LUTs (dv_mmr64_build_lut) */
    unsigned char reshape[4544];        /* dv_single_reshape, the GPU struct as is */
    int32_t residual[3][1024];          /* luma, Cb, Cr residual tables */
    uint32_t mmr_params[2][96];
    uint32_t route, width, height, el_scale, nearest, chroma_phase;
    uint32_t chroma_kind[2], dual, horizontal_fused, equal;
    uint32_t row_first, row_last;       /* letterbox window (8-aligned), 0,0 = all rows */
    uint32_t layout, sampling, depth, order2, narrow_mask[4];
    uint32_t pack, active[4], black[4], placement[4], top_left, source_w, source_h;
    uint32_t packet_count, overlay_visible;
    uint32_t chroma_filter;             /* sited pack: 0 the 2-tap blend, 1 Annex B (CB1) */
    uint32_t tables_generation;         /* _state: changes whenever the tables (mmr_lut, reshape,
                                         * residual, mmr_params, mapping, chroma_mapping,
                                         * single_table, luma_table) may differ from the
                                         * previous frame's; equal: the same bytes */
    uint32_t dm_status;                 /* DVVK_CM4_STATUS_* | L2 blocks removed << 8 */
    uint32_t reserved[4];
    float gui_coeff[32];                /* overlay colour, as prepare_overlay() builds it */
    uint16_t mapping[1024];             /* luma mapping (Q16) */
    uint16_t chroma_mapping[2][1024];   /* polynomial chroma (Q16) */
    uint16_t single_table[3][1024];     /* single-layer 12-bit tables */
    uint16_t luma_table[1024];          /* piecewise luma table (12-bit) */
    unsigned char packets[640];
} dvvk_plan;

/* Desktop mode: static graphics display management for frames the desktop
 * fills (the film in a small box). 0 fields: the defaults (min PQ(0.005), max
 * the film's mastering peak but at least PQ(white_nits), avg midway between
 * min and the white). */
typedef struct {
    uint32_t enabled;
    uint32_t min_pq, max_pq, avg_pq;    /* 12-bit PQ codes */
} dvvk_plan_desktop;

/* What one engine's planning keeps from frame to frame: CB1's packet id
 * continuity (the contract advances the id only when the payload changes)
 * and the tables of the last composer configuration per route, so a frame
 * whose configuration didn't change copies them instead of planning them
 * again (P1). New and reset: as a new engine (dvvk_reset). */
typedef struct dvvk_plan_state dvvk_plan_state;
dvvk_plan_state *dvvk_plan_state_new(void);
void dvvk_plan_state_reset(dvvk_plan_state *state);
void dvvk_plan_state_free(dvvk_plan_state *state);

/* 0 OK; else a dv_status (-1 invalid, -2 unsupported: ordinary picture, ...).
 * dvvk_plan_frame and _ex keep one state per process (tests); the engine
 * passes its own to _state. */
int dvvk_plan_frame(const dvvk_plan_in *in, const void *metadata, size_t metadata_size, dvvk_plan *out);
int dvvk_plan_frame_ex(const dvvk_plan_in *in, const dvvk_plan_desktop *desktop, const void *metadata,
                       size_t metadata_size, dvvk_plan *out);
int dvvk_plan_frame_state(const dvvk_plan_in *in, const dvvk_plan_desktop *desktop, dvvk_plan_state *state,
                          const void *metadata, size_t metadata_size, dvvk_plan *out);
/* With the picture's raw RPU (AV_FRAME_DATA_DOVI_RPU_BUFFER's data and size;
 * NULL: none), which the CM v4.0 levels are re-encoded from: without it a
 * picture carrying them is refused (DV_UNSUPPORTED). From the same AVFrame
 * as the metadata. */
int dvvk_plan_frame_state_rpu(const dvvk_plan_in *in, const dvvk_plan_desktop *desktop, dvvk_plan_state *state,
                              const void *metadata, size_t metadata_size, const void *rpu, size_t rpu_size,
                              dvvk_plan *out);
/* sizeof and a few offsets, for the Rust mirror. */
void dvvk_plan_layout(size_t out[6]);

/* The layouts (LP64), as src/abi.rs asserts them for the Rust mirrors. */
#ifdef __cplusplus
#define DVVK_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define DVVK_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
#if UINTPTR_MAX == UINT64_MAX
DVVK_STATIC_ASSERT(sizeof(dvvk_plan_in) == 160, "dvvk_plan_in layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan_in, pts) == 64, "dvvk_plan_in layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan_in, rgb_to_lms) == 72, "dvvk_plan_in layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan_in, white_nits) == 144, "dvvk_plan_in layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan_in, cb1_contract) == 152, "dvvk_plan_in layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan_in, dm_options) == 156, "dvvk_plan_in layout");
DVVK_STATIC_ASSERT(sizeof(dvvk_plan) == 82040, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, reshape) == 49152, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, residual) == 53696, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, mmr_params) == 65984, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, route) == 66752, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, packet_count) == 66900, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, chroma_filter) == 66908, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, tables_generation) == 66912, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, dm_status) == 66916, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, gui_coeff) == 66936, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, mapping) == 67064, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, chroma_mapping) == 69112, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, single_table) == 73208, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, luma_table) == 79352, "dvvk_plan layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_plan, packets) == 81400, "dvvk_plan layout");
DVVK_STATIC_ASSERT(sizeof(dvvk_plan_desktop) == 16, "dvvk_plan_desktop layout");
#endif
