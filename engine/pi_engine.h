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
 *   pi_overlay_import           optional: the caller's subtitle buffers
 *   pi_render                   per picture, into an output; returns a
 *                               sync_file for the display (or wait on it)
 *   pi_stream_close             waits, releases the stream's imports and frames
 *   pi_output_release, pi_engine_destroy
 * One thread drives an engine. Up to PI_FRAMES_IN_FLIGHT renders are in
 * flight; pi_render waits for the oldest beyond that.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavutil/frame.h>   /* FFmpeg's headers have no C++ linkage of their own */

/* This header's version: bumped by every change that breaks a caller built
 * against the last (libpi_engine.so.PI_API_VERSION). pi_api_version() is
 * the library's. */
#define PI_API_VERSION 3
int pi_api_version(void);

#define PI_FRAMES_IN_FLIGHT 3
#define PI_OVERLAY_RECTS 4

struct pi_engine;
struct pi_output;
struct pi_overlay;

/* 0 OK; < 0 an error. */
enum pi_status {
    PI_OK = 0,
    PI_E_INVALID = -1,            /* a bad argument, or a call out of order */
    PI_E_UNSUPPORTED_FORMAT = -2, /* the input isn't a DRM PRIME SAND128 frame of the stream's size */
    PI_E_UNSUPPORTED_PLAN = -3,   /* the planner refuses the stream, or plans outside these passes */
    PI_E_BAD_RPU = -4,            /* the frame has no RPU, or the planner finds it invalid */
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

/* The FEL's EL upscale (2x), at the same sample positions:
 *   BILINEAR  the default: vertical 3:1, horizontal 1:1 (yblod's)
 *   ANNEX_B   ETSI GS CCM 001 Annex B's example: luma a 4-tap vertical and
 *             an 8-tap horizontal filter, chroma 64/192 vertical and the
 *             8-tap horizontal
 *   BICUBIC   Catmull-Rom in both directions, luma and chroma: vertical
 *             -9 111 29 -3 / 128 (Annex B's luma vertical), horizontal
 *             -1 9 9 -1 / 16
 *   LANCZOS   Lanczos-3 in both directions, luma and chroma: vertical
 *             8 -70 277 914 -136 31 / 1024, horizontal
 *             25 -139 626 626 -139 25 / 1024
 * Every option but bilinear reads more of the EL in both passes. */
enum pi_el_upscale { PI_EL_BILINEAR = 0, PI_EL_ANNEX_B = 1, PI_EL_BICUBIC = 2, PI_EL_LANCZOS = 3 };

enum pi_log_level { PI_LOG_ERROR = 0, PI_LOG_WARNING = 1, PI_LOG_INFO = 2, PI_LOG_DEBUG = 3 };

/* Measurement only (pi_options.measure). */
enum {
    PI_MEASURE_ALL_ROWS = 1,      /* pass 1 and 2 over every row (no letterbox skipping) */
    PI_MEASURE_DRAW_BARS = 2,     /* draw the letterbox bars instead of clearing them */
    PI_MEASURE_PIECE_GENERAL = 4, /* route 3: always the general reshape */
    PI_MEASURE_MARK = 8,          /* mark the MMR's fallback samples (p1_chroma.frag MARK) */
    PI_MEASURE_COPY_OUTPUT = 16,  /* render into own images and copy (no DRM format modifiers) */
    PI_MEASURE_FORCE_WIDE = 32,   /* compute 64-bit LUT MMRs as MMR128 (the 128-bit path's test) */
    PI_MEASURE_EL_OUT = 64,       /* pass 1 writes the upscaled EL chroma as its chroma (the EL upscale's test) */
    PI_MEASURE_NO_QUAD = 128,     /* no subgroup quad sharing (the path of devices without it) */
};

struct pi_options {
    const char *device;           /* substring of the Vulkan device's name; NULL: the first GPU */
    enum pi_mmr_mode mmr;
    enum pi_filters filters;
    enum pi_el_upscale el_upscale;
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

/* What pi_engine_create would find, without creating an engine (a Vulkan
 * instance, no device): for a player's capability check. o: its device and
 * measure options (NULL: the defaults). 0 a device the engine can use;
 * PI_E_VULKAN none. */
struct pi_probe_info {
    char device[256];             /* the Vulkan device's name */
    int cpu;                      /* a CPU implementation (llvmpipe): not real time */
    int in_place;                 /* renders into the output (else into its own image, then copies) */
    int release_fence;            /* pi_render returns a sync_file */
};
int pi_probe(const struct pi_options *o, struct pi_probe_info *info);
/* Waits for the GPU, releases every object, writes the pipeline cache. */
void pi_engine_destroy(struct pi_engine *e);
/* Changes o's mmr, filters and el_upscale (the rest of o is ignored), from
 * the next pi_stream_open: a player applies a change by reopening the
 * stream. PI_E_INVALID for a value out of range. */
int pi_engine_configure(struct pi_engine *e, const struct pi_options *o);
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

/* Graphics over the picture (subtitles), composited into its Dolby Vision
 * signal: an overlay is the caller's linear ARGB8888 dmabuf
 * (DRM_FORMAT_ARGB8888: B, G, R, A bytes) of sRGB with premultiplied alpha,
 * width x height pixels, stride bytes a row; it is scaled to the output
 * (bilinear) and composited at 203 nits for white. While one is shown the
 * metadata's active area (L5) is the whole picture, so the TV maps the
 * graphics as it does the picture. fd stays the caller's. */
int pi_overlay_import(struct pi_engine *e, int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t size,
                      struct pi_overlay **out);
/* Waits for the renders reading it first. */
void pi_overlay_release(struct pi_engine *e, struct pi_overlay *o);

struct pi_frame {
    const AVFrame *bl, *el;       /* AV_PIX_FMT_DRM_PRIME, SAND128 P030; el NULL: none */
    /* The RPU: the side data FFmpeg's HEVC decoder attaches,
     * AV_FRAME_DATA_DOVI_METADATA and AV_FRAME_DATA_DOVI_RPU_BUFFER (which
     * the CM v4.0 levels are re-encoded from), of rpu_frame; NULL: of bl, or
     * else of el. rpu_frame is for an EL frame carrying it that isn't
     * rendered (el NULL). */
    const AVFrame *rpu_frame;
    int chroma_top_left;          /* pi_chroma_top_left() */
    int64_t pts_us;
    int acquire_fd;               /* a sync_file the output is busy until (KMS); -1 none. Taken. */
    /* The overlay (pi_overlay_import) composited over the picture; NULL or
     * only empty rectangles: none. The engine reads it until the render's
     * release sync_file signals: draw into another one meanwhile. */
    struct pi_overlay *overlay;
    /* Up to PI_OVERLAY_RECTS rectangles, x0, y0, x1, y1 (exclusive), the
     * overlay's pixels, rows top first: where it may be other than
     * transparent (it is ignored outside). Empty ones are skipped; the
     * composite's cost follows their area (a GUI's top and bottom bars:
     * two). */
    uint32_t overlay_rect[PI_OVERLAY_RECTS][4];
    int overlay_bottom_up;        /* the overlay's rows are stored bottom first (a GL framebuffer) */
    /* With an overlay: a sync_file it is drawn by; -1 none. Taken. The same
     * overlay passed again with the same rectangles, row order and drawing
     * (this sync_file or a copy of it) is taken as unchanged: its conversion
     * is reused, and from its second picture on the picture under its opaque
     * areas is neither reconstructed nor read. An overlay drawn again comes
     * with the new drawing's sync_file. */
    int overlay_fd;
};
/* Plans and renders f into out. out is of the stream's width and its
 * height or taller: a taller one (a stream cropped to the picture, such as
 * 3840x2076, in a 3840x2160 tunnel frame) gets the picture in its middle,
 * from an even row, with black bars above and below, the metadata packets in
 * its first rows and the active area (L5) placed. The engine holds
 * references to bl and el until the GPU is done with them. release_fd (may be NULL): a sync_file
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

/* Whether the picture needs its EL: 1 its RPU has a residual for the EL to
 * carry (FEL), so rendering it without one is wrong; 0 not (MEL, whose
 * residual is zero, or a single-layer profile), so the EL may be left out
 * and its decoder closed; PI_E_BAD_RPU no RPU. f: the frame carrying the RPU
 * (pi_frame.rpu_frame's). */
int pi_frame_needs_el(const AVFrame *f);

/* Pairs a dual-layer stream's BL and EL pictures, from two decoders, by
 * display time: the BL picture's timestamp, and the EL picture's as its
 * position among the EL decoder's output gives it (an EL picture's own pts
 * is its packet's, not its display time, where the EL's coding order
 * differs). Without timestamps, in output order. A picture whose partner a
 * decoder lost: an EL is dropped; a BL is dropped when it needs its EL
 * (pi_frame_needs_el), else given alone. A stream whose EL decoder gives no
 * pictures (none by the BL's 8th) is given BL alone from then on
 * (pi_pair_stats.el_absent: its decoder may be closed). layers 1: BLs
 * through as they come. One thread drives a pairer. */
struct pi_pairer;
int pi_pairer_create(int layers, struct pi_pairer **out);
void pi_pairer_destroy(struct pi_pairer *p);
/* Pictures the pairer can take of the layer (0 BL, 1 EL) before a pop. */
int pi_pairer_room(const struct pi_pairer *p, int layer);
/* A decoder's next picture, in its output order. 0: taken (owned by the
 * pairer from then on); 1: full, not taken (pop first); < 0 an error.
 * el_reorder: the EL decoder's AVCodecContext.has_b_frames (EL only). */
int pi_pairer_push(struct pi_pairer *p, int layer, AVFrame *f, int el_reorder);
/* The next pair: 1, *bl and *el (NULL: none) the caller's; 0 none yet.
 * end: the decoders are flushed, no more pictures will come. */
int pi_pairer_pop(struct pi_pairer *p, int end, AVFrame **bl, AVFrame **el);
/* Drops every picture held: on a seek, after flushing the decoders. */
void pi_pairer_flush(struct pi_pairer *p);
struct pi_pair_stats {
    int el_absent;                /* no EL pictures: BLs alone */
    unsigned bl_alone;            /* BLs given without their EL (MEL) */
    unsigned bl_dropped;          /* BLs dropped without their EL (FEL) */
    unsigned el_dropped;          /* ELs without their BL */
};
void pi_pairer_stats(const struct pi_pairer *p, struct pi_pair_stats *s);

#ifdef __cplusplus
}
#endif
