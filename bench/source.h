/* Decoded Dolby Vision pictures as DRM PRIME SAND128 frames, from either
 *   - the Raspberry Pi 5's V4L2 request HEVC decoder (FFmpeg hwaccel), whose
 *     frames are SAND128 (P030 10-bit) dmabufs, or
 *   - FFmpeg's software decoder, each picture copied into a dmabuf
 *     (alloc.h) in the Pi decoder's exact SAND128 P030 layout, so the
 *     engine's input path runs on any Linux box.
 * Profile 7's BL and EL are decoded separately (single-track: split by
 * pi_p7_split) and paired in output order (source.c take()). This is the
 * template for a player's input glue (docs/REVIEW.md P1 #8).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdint.h>

enum dv_source_kind { DV_SOURCE_SW, DV_SOURCE_V4L2 };

struct dv_source_options {
    enum dv_source_kind kind;
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
