/* The Raspberry Pi 5 Dolby Vision engine: Profile 5, 7 (FEL and MEL) and 8.x
 * reconstruction into the TV-led HDMI tunnel frame, as two Vulkan fragment
 * passes built for V3D (docs/ENGINE.md, docs/pi5/V3DV.md). The CPU half is
 * yblod's planner (planner/), so the tables, metadata packets and pack
 * constants are yblod's; the passes compute what yblod's bilinear FEL route
 * computes, bit for bit, in every MMR mode but `float`.
 *
 * Use, for a long-lived caller (a player):
 *   pi_engine_create            once
 *   pi_stream_open              per stream: its size; resets the planner
 *   pi_output_import            the caller's scanout buffers (pi_output_size)
 *   pi_render                   per picture, into an output; returns a
 *                               sync_file for the display (or wait on it)
 *   pi_stream_close             waits, releases the stream's imports and frames
 *   pi_output_release, pi_engine_destroy
 * One thread drives an engine. Up to PI_FRAMES_IN_FLIGHT renders are in
 * flight; pi_render waits for the oldest beyond that.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <libavutil/frame.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PI_FRAMES_IN_FLIGHT 3

struct pi_engine;
struct pi_output;

/* 0 OK; < 0 an error. */
enum pi_status {
    PI_OK = 0,
    PI_E_INVALID = -1,            /* a bad argument, or a call out of order */
    PI_E_UNSUPPORTED_FORMAT = -2, /* the input isn't a DRM PRIME SAND128 frame of the stream's size */
    PI_E_UNSUPPORTED_PLAN = -3,   /* the planner refuses the stream, or plans outside these passes */
    PI_E_BAD_RPU = -4,            /* the planner finds the RPU invalid */
    PI_E_VULKAN = -5,
    PI_E_NOMEM = -6,
};
const char *pi_strerror(int status);

/* The chroma MMR (docs/ENGINE.md "Chroma MMR modes"):
 *   EXACT    the default: float with a per-sample error bound, the split
 *            where the bound could change the 12-bit result
 *   SPLIT    the 64-bit sum's low word in 32-bit integers, its high word
 *            from a float sum
 *   EXACT64  emulated 64-bit (yblod's)
 *   FLOAT    32-bit float: within 2 codes, not exact
 * Every mode but FLOAT is exact. MMR96 / MMR128 curves (beyond the 64-bit
 * LUT) are emulated 128-bit in every exact mode. */
enum pi_mmr_mode { PI_MMR_FLOAT = 0, PI_MMR_EXACT64 = 2, PI_MMR_SPLIT = 3, PI_MMR_EXACT = 4 };

/* The spatial filters:
 *   CB1    the default: CB1 Master Reference's contract (4:2:0 reshaping, no
 *          chroma phase filter, Annex B 4:2:2 filters)
 *   PHASE  yblod's chroma phase filters, the 2-tap 4:2:2 blend and the
 *          full-height single-layer MMR layout; the planner refuses
 *          single-layer MMR (Profile 8.1 MMR) under CB1 */
enum pi_filters { PI_FILTERS_CB1 = 0, PI_FILTERS_PHASE = 1 };

enum pi_log_level { PI_LOG_ERROR = 0, PI_LOG_WARNING = 1, PI_LOG_INFO = 2, PI_LOG_DEBUG = 3 };

/* Measurement only (pi_options.measure). */
enum {
    PI_MEASURE_ALL_ROWS = 1,      /* pass 1 and 2 over every row (no letterbox skipping) */
    PI_MEASURE_DRAW_BARS = 2,     /* draw the letterbox bars instead of clearing them */
    PI_MEASURE_PIECE_GENERAL = 4, /* route 3: always the general reshape */
    PI_MEASURE_MARK = 8,          /* mark the MMR's fallback samples (p1_chroma.frag MARK) */
    PI_MEASURE_COPY_OUTPUT = 16,  /* render into own images and copy (no DRM format modifiers) */
    PI_MEASURE_FORCE_WIDE = 32,   /* compute 64-bit LUT MMRs as MMR128 (the 128-bit path's test) */
};

struct pi_options {
    const char *device;           /* substring of the Vulkan device's name; NULL: the first GPU */
    enum pi_mmr_mode mmr;
    enum pi_filters filters;
    const char *pipeline_cache;   /* a file: read at create, written at destroy; NULL: none */
    /* Messages: NULL log: stderr. Levels above log_level are dropped. */
    void (*log)(void *opaque, enum pi_log_level level, const char *message);
    void *log_opaque;
    enum pi_log_level log_level;
    int profile;                  /* GPU timestamps per pass (pi_output_stats) */
    unsigned measure;             /* PI_MEASURE_* */
    unsigned ablate;              /* p2_pack.frag ABLATE: measurement only */
};
void pi_options_default(struct pi_options *o);

int pi_engine_create(const struct pi_options *o, struct pi_engine **out);
/* Waits for the GPU, releases every object, writes the pipeline cache. */
void pi_engine_destroy(struct pi_engine *e);
/* "device, MMR mode, output path", for logs. */
const char *pi_engine_describe(const struct pi_engine *e);

struct pi_stream_info {
    uint32_t width, height;       /* the decoded pictures' (even) */
    int profile;                  /* the Dolby Vision profile (pi_chroma_top_left) */
};
/* Closes the open stream first. Resets the planner (packet continuity,
 * cached tables) and builds the stream's pipelines. */
int pi_stream_open(struct pi_engine *e, const struct pi_stream_info *info);
/* Waits for the stream's renders; releases its decoder imports and frames. */
void pi_stream_close(struct pi_engine *e);

/* The output buffer a width x height stream renders into: linear XRGB8888
 * (the tunnel frame's BGRA bytes), stride bytes a row, at least bytes long
 * (a row more than the picture, plus a page for drivers' padding). */
int pi_output_size(uint32_t width, uint32_t height, uint32_t *stride, uint64_t *bytes);
/* Imports the caller's dmabuf as an output of that size; fd stays the
 * caller's. Validated against pi_output_size's rules. */
int pi_output_import(struct pi_engine *e, int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t size,
                     struct pi_output **out);
/* Waits for the output's last render first. */
void pi_output_release(struct pi_engine *e, struct pi_output *o);

struct pi_frame {
    const AVFrame *bl, *el;       /* AV_PIX_FMT_DRM_PRIME, SAND128 P030; el NULL: none */
    const void *rpu;              /* AV_FRAME_DATA_DOVI_METADATA's data, and its size */
    size_t rpu_size;
    int chroma_top_left;          /* pi_chroma_top_left() */
    int64_t pts_us;
    int acquire_fd;               /* a sync_file the output is busy until (KMS); -1 none. Taken. */
};
/* Plans and renders f into out. The engine holds references to bl and el
 * until the GPU is done with them. release_fd (may be NULL): a sync_file
 * signalled when the output is complete, or -1 where the device can't
 * export one (then pi_output_wait). */
int pi_render(struct pi_engine *e, const struct pi_frame *f, struct pi_output *out, int *release_fd);
/* The output's last render: 1 done (or none), 0 still running. */
int pi_output_done(struct pi_engine *e, struct pi_output *o);
int pi_output_wait(struct pi_engine *e, struct pi_output *o);

struct pi_render_stats {
    double pass_ms[2];            /* GPU time per pass (pi_options.profile), else 0 */
    double plan_ms;               /* the planner's CPU time */
};
/* Of the output's last finished render. */
void pi_output_stats(struct pi_engine *e, struct pi_output *o, struct pi_render_stats *s);

/* ---- input glue: what a player needs around the engine (pi_input.c) ---- */

/* The stream's chroma siting, as Kodi's DV adapters read it: Profile 7
 * without one is top-left. */
int pi_chroma_top_left(const AVFrame *f, int profile);
/* DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(h): the Pi decoder's layout. */
int pi_is_sand128(uint64_t modifier);
/* Single-track Profile 7 (MKV/MP4, length-prefixed NAL units of nal_size
 * bytes): layer 0 (BL) keeps every NAL unit but type 63; layer 1 (EL) is
 * each type-63 unit's payload, Annex B. Writes at most room bytes to out;
 * returns the bytes written, or -1. */
int pi_p7_split(const uint8_t *in, int size, int nal_size, int layer, uint8_t *out, int room);

#ifdef __cplusplus
}
#endif
