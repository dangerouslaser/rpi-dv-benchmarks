/* Decoded Dolby Vision pictures as DRM PRIME frames, from either
 *   - the Raspberry Pi 5's V4L2 request HEVC decoder (FFmpeg hwaccel), whose
 *     frames are SAND128 (P030 10-bit) dmabufs, or
 *   - FFmpeg's software decoder, each picture copied into a udmabuf laid out
 *     as linear P010 or as the Pi decoder's exact SAND128 P030 layout, so both
 *     engines' Pi input paths run on any Linux box with /dev/udmabuf.
 * The BL and (Profile 7) the EL are decoded separately and paired in order,
 * as dvvk-mr-compare's decode.c does (single-track P7 split here).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>
#include <stdint.h>

enum dv_source_kind { DV_SOURCE_SW, DV_SOURCE_V4L2 };
enum dv_layout { DV_LAYOUT_P010, DV_LAYOUT_SAND30 };

struct dv_source_options {
    enum dv_source_kind kind;
    enum dv_layout layout;      /* DV_SOURCE_SW: the layout the copies get */
    int el_software;            /* V4L2: decode the EL in software (one HW decoder) */
    int extra_frames;           /* V4L2: pictures the caller may hold beyond the decoder's own */
    double start;
};

struct dv_source;

struct dv_picture {
    AVFrame *bl, *el;           /* AV_PIX_FMT_DRM_PRIME; el NULL if none */
    const AVFrameSideData *dovi; /* AV_FRAME_DATA_DOVI_METADATA */
};

struct dv_source *dv_source_open(const char *path, const struct dv_source_options *o);
int dv_source_profile(const struct dv_source *s);
AVRational dv_source_rate(const struct dv_source *s);
/* 1: a picture; 0: end; <0: error. */
int dv_source_next(struct dv_source *s, struct dv_picture *p);
void dv_picture_free(struct dv_picture *p);
/* CPU time spent copying software pictures into dmabufs (ms), so far. */
double dv_source_copy_ms(const struct dv_source *s);
void dv_source_close(struct dv_source *s);

static inline const AVDRMFrameDescriptor *dv_drm(const AVFrame *f)
{
    return (const AVDRMFrameDescriptor *)f->data[0];
}

/* DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(h) and its parameter. */
#define DV_MOD_SAND128(h) ((0x07ull << 56) | ((uint64_t)(h) << 8) | 4ull)
static inline int dv_is_sand128(uint64_t m) { return (m >> 56) == 0x07 && (m & 0xff) == 4; }
static inline uint32_t dv_sand_height(uint64_t m) { return (uint32_t)((m >> 8) & 0xffffffffffffull); }
