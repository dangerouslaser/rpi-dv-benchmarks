/* See source.h.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "source.h"
#include "alloc.h"
#include "pi_engine.h"
#include <drm_fourcc.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QUEUE 32   /* decoded pictures held per layer */
#define POOL 160   /* the software path's dmabufs */

/* A dmabuf the software path copies pictures into; reused once released. */
struct pooled {
    struct dv_dmabuf buf;
    _Atomic int busy;   /* released by whichever thread frees the picture */
};

struct dv_source {
    struct dv_source_options o;
    AVFormatContext *fmt;
    int stream;
    AVRational time_base;
    AVBufferRef *device;
    AVCodecContext *dec[2];
    AVPacket *pending[2];   /* a packet the decoder couldn't take yet (its output queue full) */
    AVFrame *queue[2][QUEUE];
    int queued[2];
    int el_out, drop_el;   /* EL pictures output; test: discard the drop_el-th (DVBENCH_DROP_EL) */
    int el_timed;   /* EL pictures at the queue's front whose pts is their display time */
    int profile, el_present, el_seen, eof, nal_size, warned;
    struct pooled pool[POOL];
    double copy_ms;
};

static double cpu_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- software pictures in dmabufs ------------------------------------------ */

static struct pooled *pool_get(struct dv_source *s, size_t size)
{
    struct pooled *free_slot = NULL;
    for (int i = 0; i < POOL; i++) {
        struct pooled *u = &s->pool[i];
        if (!u->busy && u->buf.map && u->buf.size == size) {
            u->busy = 1;
            return u;
        }
        if (!u->buf.map && !free_slot)
            free_slot = u;
    }
    if (!free_slot) {
        fprintf(stderr, "source: dmabuf pool exhausted (%d pictures held)\n", POOL);
        return NULL;
    }
    if (dv_dmabuf_alloc(&free_slot->buf, size))
        return NULL;
    free_slot->busy = 1;
    return free_slot;
}

static void pool_release(void *opaque, uint8_t *data)
{
    ((struct pooled *)opaque)->busy = 0;
    av_free(data);
}

static inline size_t align(size_t v, size_t a) { return (v + a - 1) / a * a; }

/* yuv420p10 sample, 10-bit. */
static inline unsigned sample(const AVFrame *f, int plane, int x, int y)
{
    const uint16_t *row = (const uint16_t *)(f->data[plane] + (size_t)y * f->linesize[plane]);
    return row[x] & 0x3ff;
}

/* SAND128 P030: 128-byte columns `colh` rows tall; three 10-bit samples per
 * 32-bit word; luma rows 0..h-1, chroma (U V interleaved, w samples a row)
 * from row `luma_rows` in each column. As the Pi's V4L2_PIX_FMT_NV12_10_COL128. */
static void write_sand30(const AVFrame *f, uint8_t *dst, uint32_t colh, uint32_t luma_rows)
{
    int w = f->width, h = f->height;
    for (int plane = 0; plane < 2; plane++) {
        int rows = plane ? h / 2 : h;
        size_t base = plane ? (size_t)luma_rows * 128 : 0;
        for (int y = 0; y < rows; y++) {
            for (int n0 = 0; n0 < w; n0 += 3) {
                uint32_t word = 0;
                for (int k = 0; k < 3 && n0 + k < w; k++) {
                    int n = n0 + k;
                    unsigned v = plane ? sample(f, 1 + (n & 1), n >> 1, y) : sample(f, 0, n, y);
                    word |= (uint32_t)v << (10 * k);
                }
                size_t b = (size_t)(n0 / 3) * 4;
                size_t addr = base + (b & 127) + (size_t)y * 128 + (b >> 7) * 128 * colh;
                memcpy(dst + addr, &word, 4);
            }
        }
    }
}

/* A software-decoded picture as a DRM PRIME frame on a dmabuf, described as
 * the Pi decoder describes its own (one object, one layer, two planes). */
static AVFrame *to_drm(struct dv_source *s, AVFrame *sw)
{
    if (sw->format != AV_PIX_FMT_YUV420P10) {
        fprintf(stderr, "source: software decoder gave %s, need yuv420p10\n", av_get_pix_fmt_name(sw->format));
        return NULL;
    }
    double c0 = cpu_now_ms();
    int w = sw->width, h = sw->height;
    uint32_t luma_rows = (uint32_t)align((size_t)h, 16), colh = luma_rows * 3 / 2;
    size_t columns = ((size_t)w + 95) / 96;
    size_t size = align(columns * 128 * colh, 4096);   /* dmabuf sizes are whole pages */
    uint64_t modifier = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(colh);
    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
    AVFrame *f = av_frame_alloc();
    struct pooled *u = desc && f ? pool_get(s, size) : NULL;
    if (!u) {
        av_free(desc);
        av_frame_free(&f);
        return NULL;
    }
    AVDRMLayerDescriptor *layer = &desc->layers[0];
    layer->format = DRM_FORMAT_P030;
    layer->nb_planes = 2;
    layer->planes[1].offset = (ptrdiff_t)luma_rows * 128;
    layer->planes[0].pitch = layer->planes[1].pitch = w * 2;   /* as FFmpeg's V4L2 request code */
    dv_dmabuf_cpu(&u->buf, 1, 1);
    write_sand30(sw, u->buf.map, colh, luma_rows);
    dv_dmabuf_cpu(&u->buf, 0, 1);
    desc->nb_objects = 1;
    desc->objects[0] = (AVDRMObjectDescriptor){.fd = u->buf.fd, .size = size, .format_modifier = modifier};
    desc->nb_layers = 1;
    f->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc), pool_release, u, 0);
    if (!f->buf[0]) {
        u->busy = 0;
        av_free(desc);
        av_frame_free(&f);
        return NULL;
    }
    f->format = AV_PIX_FMT_DRM_PRIME;
    f->width = w;
    f->height = h;
    f->data[0] = (uint8_t *)desc;
    av_frame_copy_props(f, sw);
    s->copy_ms += cpu_now_ms() - c0;
    return f;
}

/* ---- decoding ------------------------------------------------------------ */

static enum AVPixelFormat pick_drm(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    (void)c;
    for (; *fmts != AV_PIX_FMT_NONE; fmts++)
        if (*fmts == AV_PIX_FMT_DRM_PRIME)
            return *fmts;
    fprintf(stderr, "source: decoder offers no DRM PRIME output (no V4L2 request HEVC?)\n");
    return AV_PIX_FMT_NONE;
}

static AVCodecContext *open_decoder(struct dv_source *s, const AVCodecParameters *par, int hw)
{
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!c || avcodec_parameters_to_context(c, par) < 0)
        goto fail;
    if (hw) {
        c->hw_device_ctx = av_buffer_ref(s->device);
        if (!c->hw_device_ctx)
            goto fail;
        c->get_format = pick_drm;
        c->extra_hw_frames = s->o.extra_frames;
    } else {
        c->thread_count = 0;
    }
    c->pkt_timebase = s->time_base;
    if (avcodec_open2(c, codec, NULL) < 0)
        goto fail;
    return c;
fail:
    avcodec_free_context(&c);
    return NULL;
}

static void prime_parameter_sets(struct dv_source *s, const AVCodecParameters *par);

struct dv_source *dv_source_open(const char *path, const struct dv_source_options *o)
{
    struct dv_source *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->o = *o;
    s->nal_size = 4;
    if (getenv("DVBENCH_DROP_EL"))
        s->drop_el = atoi(getenv("DVBENCH_DROP_EL"));
    if (avformat_open_input(&s->fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(s->fmt, NULL) < 0)
        goto fail;
    s->stream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (s->stream < 0)
        goto fail;
    AVStream *st = s->fmt->streams[s->stream];
    s->time_base = st->time_base;
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (sd) {
        const AVDOVIDecoderConfigurationRecord *cfg = (const void *)sd->data;
        s->profile = cfg->dv_profile;
        s->el_present = cfg->el_present_flag && cfg->dv_profile == 7;
    }
    int hw = o->kind == DV_SOURCE_V4L2;
    if (hw && av_hwdevice_ctx_create(&s->device, AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0) < 0) {
        fprintf(stderr, "source: no DRM hw device\n");
        goto fail;
    }
    const AVCodecParameters *par = st->codecpar;
    s->dec[0] = open_decoder(s, par, hw);
    if (!s->dec[0])
        goto fail;
    if (s->el_present) {
        if (par->extradata_size < 23 || par->extradata[0] != 1) {
            fprintf(stderr, "source: Profile 7 needs hvcC (MKV/MP4)\n");
            goto fail;
        }
        s->nal_size = (par->extradata[21] & 3) + 1;
        AVCodecParameters *elp = avcodec_parameters_alloc();
        if (!elp || avcodec_parameters_copy(elp, par) < 0) {
            avcodec_parameters_free(&elp);
            goto fail;
        }
        av_freep(&elp->extradata);
        elp->extradata_size = 0;
        s->dec[1] = open_decoder(s, elp, hw && !o->el_software);
        avcodec_parameters_free(&elp);
        if (!s->dec[1])
            goto fail;
    }
    if (o->start > 0) {
        prime_parameter_sets(s, par);
        av_seek_frame(s->fmt, s->stream, (int64_t)(o->start / av_q2d(s->time_base)), AVSEEK_FLAG_BACKWARD);
    }
    return s;
fail:
    dv_source_close(s);
    return NULL;
}

/* Some hev1 files (Dolby's own Sol Levante samples among them) carry their
 * VPS/SPS/PPS only in the first access unit, not in the hvcC record. A decoder
 * that starts at a later keyframe then has no parameter sets. A player decodes
 * from the start and keeps them across seeks; do the same before --start:
 * give the base-layer decoder the first packet's parameter sets alone. */
static void prime_parameter_sets(struct dv_source *s, const AVCodecParameters *par)
{
    if (par->extradata_size < 23 || par->extradata[0] != 1 || par->extradata[22] != 0)
        return; /* not hvcC, or hvcC with its own parameter sets */
    int len = (par->extradata[21] & 3) + 1;
    AVPacket *pkt = av_packet_alloc(), *ps = av_packet_alloc();
    while (pkt && av_read_frame(s->fmt, pkt) >= 0 && pkt->stream_index != s->stream)
        av_packet_unref(pkt);
    if (pkt && ps && pkt->size && av_new_packet(ps, pkt->size) >= 0) {
        int n = 0;
        for (int i = 0; i + len <= pkt->size;) {
            uint32_t size = 0;
            for (int k = 0; k < len; k++)
                size = size << 8 | pkt->data[i + k];
            if (size == 0 || size > (uint32_t)(pkt->size - i - len))
                break;
            int type = pkt->data[i + len] >> 1 & 0x3f;
            if (type >= 32 && type <= 34) { /* VPS, SPS, PPS */
                memcpy(ps->data + n, pkt->data + i, len + size);
                n += len + (int)size;
            }
            i += len + (int)size;
        }
        if (n) {
            ps->size = n;
            avcodec_send_packet(s->dec[0], ps);
        }
    }
    av_packet_free(&ps);
    av_packet_free(&pkt);
}

int dv_source_profile(const struct dv_source *s) { return s->profile; }
double dv_source_copy_ms(const struct dv_source *s) { return s->copy_ms; }

AVRational dv_source_rate(const struct dv_source *s)
{
    return s->fmt->streams[s->stream]->avg_frame_rate;
}

/* The EL pictures' display times. An EL picture's pts is its packet's, which
 * isn't its display time where the EL's coding order differs from the
 * packets' (the "All Layers" test clip). But the decoder outputs in display
 * order, and the output pictures' packet times, within the EL's reorder
 * depth, are their display times in another order: the earliest of those not
 * yet given out is the next picture's. Pictures the decoder discarded
 * (leading pictures after a seek) never come out, so they take no time. At
 * the end (flush) every picture is given one. */
static void el_display_times(struct dv_source *s, int flush)
{
    int depth = s->dec[1] ? s->dec[1]->has_b_frames + 2 : 1;
    if (depth > QUEUE / 2)
        depth = QUEUE / 2;
    while (s->el_timed < s->queued[1] && (flush || s->queued[1] - s->el_timed > depth)) {
        AVFrame **q = s->queue[1];
        int m = s->el_timed;
        for (int i = s->el_timed + 1; i < s->queued[1]; i++)
            if (q[i]->pts != AV_NOPTS_VALUE && (q[m]->pts == AV_NOPTS_VALUE || q[i]->pts < q[m]->pts))
                m = i;
        int64_t t = q[m]->pts;   /* the earliest: this picture's; it gives its own to the one it came from */
        q[m]->pts = q[s->el_timed]->pts;
        q[s->el_timed++]->pts = t;
    }
}

/* Decoded pictures into the layer's queue, until it is full or the decoder
 * has none ready. */
static int drain_decoder(struct dv_source *s, int layer)
{
    while (s->queued[layer] < QUEUE) {
        AVFrame *f = av_frame_alloc();
        if (!f)
            return AVERROR(ENOMEM);
        int r = avcodec_receive_frame(s->dec[layer], f);
        if (r < 0) {
            av_frame_free(&f);
            return r == AVERROR(EAGAIN) || r == AVERROR_EOF ? 0 : r;
        }
        if (f->format != AV_PIX_FMT_DRM_PRIME) {
            AVFrame *d = to_drm(s, f);
            av_frame_free(&f);
            if (!d)
                return AVERROR(EINVAL);
            f = d;
        }
        if (layer) {
            s->el_seen = 1;
            if (++s->el_out == s->drop_el) {   /* a decoder losing a picture, for the pairing's test */
                av_frame_free(&f);
                continue;
            }
        }
        s->queue[layer][s->queued[layer]++] = f;
        if (layer)
            el_display_times(s, 0);
    }
    return 0;
}

/* Sends pkt (NULL: flush) to the layer's decoder. 0 sent; 1 not taken (the
 * layer's queue is full: send it again once pictures are taken); <0 error. */
static int send_layer(struct dv_source *s, int layer, const AVPacket *pkt)
{
    for (;;) {
        int r = avcodec_send_packet(s->dec[layer], pkt);
        if (r == AVERROR(EAGAIN)) {
            int d = drain_decoder(s, layer);
            if (d < 0)
                return d;
            if (s->queued[layer] >= QUEUE)
                return 1;
            continue;
        }
        if (r < 0 && r != AVERROR_EOF)
            return r;
        return drain_decoder(s, layer);
    }
}

/* The layer's share of a stream packet: single-layer, the packet; Profile 7,
 * pi_p7_split's. NULL if empty. */
static AVPacket *layer_packet(struct dv_source *s, int layer, const AVPacket *in)
{
    AVPacket *out = av_packet_alloc();
    if (!out)
        return NULL;
    if (!s->dec[1]) {
        if (av_packet_ref(out, in) < 0)
            av_packet_free(&out);
        return out;
    }
    int room = in->size + 64 * 4;
    int n = av_new_packet(out, room) < 0 ? -1 : pi_p7_split(in->data, in->size, s->nal_size, layer, out->data, room);
    if (n <= 0 || av_packet_copy_props(out, in) < 0) {
        av_packet_free(&out);
        return NULL;
    }
    out->size = n;
    return out;
}

static AVFrame *pop(struct dv_source *s, int layer)
{
    AVFrame *f = s->queue[layer][0];
    if (layer && s->el_timed)
        s->el_timed--;
    memmove(s->queue[layer], s->queue[layer] + 1, sizeof(s->queue[0][0]) * (size_t)--s->queued[layer]);
    return f;
}

/* The next picture from the queues; 0 if none can be made yet. The layers
 * pair by display time: the BL picture's timestamp, and the EL picture's as
 * el_display_times assigns it. A picture without its partner (a decoder lost
 * one) is dropped (EL) or shown alone (BL). Without timestamps: in output
 * order, as both decoders output in display order. */
static int take(struct dv_source *s, struct dv_picture *p, int layers)
{
    while (s->queued[0]) {
        if (layers == 2 && s->eof == 2)
            el_display_times(s, 1);
        if (layers == 2 && !s->el_timed && s->queued[0] < QUEUE && s->eof < 2)
            return 0;   /* wait for its EL */
        if (layers == 2 && s->el_timed) {
            int64_t a = s->queue[0][0]->pts, b = s->queue[1][0]->pts;
            int timed = a != AV_NOPTS_VALUE && b != AV_NOPTS_VALUE;
            if (timed && b < a) {   /* an EL picture whose BL was lost */
                AVFrame *f = pop(s, 1);
                av_frame_free(&f);
                if (!s->warned++)
                    fprintf(stderr, "source: an enhancement-layer picture without its base layer\n");
                continue;
            }
            p->bl = pop(s, 0);
            if (!timed || a == b)
                p->el = pop(s, 1);
        } else
            p->bl = pop(s, 0);
        if (layers == 2 && !p->el && !s->warned++)
            fprintf(stderr, "source: a base-layer picture without its enhancement layer\n");
        p->dovi = av_frame_get_side_data(p->bl, AV_FRAME_DATA_DOVI_METADATA);
        if (!p->dovi && p->el)
            p->dovi = av_frame_get_side_data(p->el, AV_FRAME_DATA_DOVI_METADATA);
        return 1;
    }
    return 0;
}

int dv_source_next(struct dv_source *s, struct dv_picture *p)
{
    memset(p, 0, sizeof(*p));
    for (;;) {
        int layers = s->dec[1] ? 2 : 1;
        if (layers == 2 && !s->el_seen && (s->queued[0] >= 8 || s->eof == 2)) {
            fprintf(stderr, "source: no enhancement-layer pictures; base layer only\n");
            avcodec_free_context(&s->dec[1]);
            av_packet_free(&s->pending[1]);
            layers = 1;
        }
        if (take(s, p, layers))
            return 1;
        /* packets the decoders couldn't take: again, now pictures are gone */
        int blocked = 0;
        for (int l = 0; l < layers; l++)
            if (s->pending[l]) {
                int r = send_layer(s, l, s->pending[l]);
                if (r < 0)
                    return r;
                if (r == 0)
                    av_packet_free(&s->pending[l]);
                blocked |= r == 1;
            }
        if (blocked) {
            if (take(s, p, layers))
                return 1;
            fprintf(stderr, "source: decoders stalled\n");
            return AVERROR(EAGAIN);
        }
        if (s->eof == 2) {   /* flushed: the decoders' last pictures, as the queues empty */
            int got = 0;
            for (int l = 0; l < layers; l++) {
                int before = s->queued[l], r = drain_decoder(s, l);
                if (r < 0)
                    return r;
                got |= s->queued[l] != before;
            }
            if (got)
                continue;
            return 0;
        }
        if (s->eof) {
            s->eof = 2;
            for (int l = 0; l < layers; l++) {
                int r = send_layer(s, l, NULL);
                if (r < 0)
                    return r;
            }
            continue;
        }
        AVPacket *pkt = av_packet_alloc();
        if (!pkt)
            return AVERROR(ENOMEM);
        if (av_read_frame(s->fmt, pkt) < 0) {
            av_packet_free(&pkt);
            s->eof = 1;
            continue;
        }
        if (pkt->stream_index == s->stream)
            for (int l = 0; l < layers; l++) {
                AVPacket *lp = layer_packet(s, l, pkt);
                if (!lp)
                    continue;
                int r = send_layer(s, l, lp);
                if (r == 1)
                    s->pending[l] = lp;
                else
                    av_packet_free(&lp);
                if (r < 0) {
                    fprintf(stderr, "source: layer %d: %s\n", l, av_err2str(r));
                    av_packet_free(&pkt);
                    return r;
                }
            }
        av_packet_free(&pkt);
    }
}

void dv_picture_free(struct dv_picture *p)
{
    av_frame_free(&p->bl);
    av_frame_free(&p->el);
}

void dv_source_close(struct dv_source *s)
{
    if (!s)
        return;
    for (int l = 0; l < 2; l++) {
        while (s->queued[l])
            av_frame_free(&s->queue[l][--s->queued[l]]);
        av_packet_free(&s->pending[l]);
        avcodec_free_context(&s->dec[l]);
    }
    av_buffer_unref(&s->device);
    avformat_close_input(&s->fmt);
    for (int i = 0; i < POOL; i++)
        if (s->pool[i].buf.map)
            dv_dmabuf_free(&s->pool[i].buf);
    free(s);
}
