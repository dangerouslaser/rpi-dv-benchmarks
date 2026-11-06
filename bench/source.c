/* See source.h. The layer split and pairing follow dvvk-mr-compare's decode.c
 * (intel-dv-libreelec yblod-engine/vk/tools).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "source.h"
#include "alloc.h"
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define QUEUE 32
#define POOL 160

/* A dmabuf the software path copies pictures into; reused once released. */
struct udma {
    struct dv_dmabuf buf;
    int busy;
};

struct dv_source {
    struct dv_source_options o;
    AVFormatContext *fmt;
    int stream;
    AVRational time_base;
    AVBufferRef *device;
    AVCodecContext *dec[2];
    AVFrame *queue[2][QUEUE];
    int queued[2];
    int profile, el_present, eof, nal_size_len;
    struct udma pool[POOL];
    double copy_ms;
};

static double cpu_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- udmabuf pictures ---------------------------------------------------- */

static struct udma *udma_get(struct dv_source *s, size_t size)
{
    struct udma *free_slot = NULL;
    for (int i = 0; i < POOL; i++) {
        struct udma *u = &s->pool[i];
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

static void udma_release(void *opaque, uint8_t *data)
{
    ((struct udma *)opaque)->busy = 0;
    av_free(data);
}

static inline size_t align(size_t v, size_t a) { return (v + a - 1) / a * a; }

/* yuv420p10 (or p010) samples, 10-bit values. */
static inline unsigned sample(const AVFrame *f, int plane, int x, int y)
{
    if (f->format == AV_PIX_FMT_P010) {
        const uint16_t *row = (const uint16_t *)(f->data[plane ? 1 : 0] + (size_t)y * f->linesize[plane ? 1 : 0]);
        return plane == 0 ? row[x] >> 6 : row[2 * x + (plane - 1)] >> 6;
    }
    const uint16_t *row = (const uint16_t *)(f->data[plane] + (size_t)y * f->linesize[plane]);
    return row[x] & 0x3ff;
}

static void write_p010(const AVFrame *f, uint8_t *dst, size_t pitch, size_t chroma_offset)
{
    int w = f->width, h = f->height;
    for (int y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(dst + (size_t)y * pitch);
        for (int x = 0; x < w; x++)
            row[x] = (uint16_t)(sample(f, 0, x, y) << 6);
    }
    for (int y = 0; y < h / 2; y++) {
        uint16_t *row = (uint16_t *)(dst + chroma_offset + (size_t)y * pitch);
        for (int x = 0; x < w / 2; x++) {
            row[2 * x] = (uint16_t)(sample(f, 1, x, y) << 6);
            row[2 * x + 1] = (uint16_t)(sample(f, 2, x, y) << 6);
        }
    }
}

/* SAND128 P030: 128-byte columns `colh` rows tall; three 10-bit samples per
 * 32-bit word; luma rows 0..h-1, chroma (U V interleaved) from row `luma_rows`
 * in each column. As the Pi's V4L2_PIX_FMT_NV12_10_COL128. */
static void write_sand30(const AVFrame *f, uint8_t *dst, uint32_t colh, uint32_t luma_rows)
{
    int w = f->width, h = f->height;
    for (int plane = 0; plane < 2; plane++) {
        int rows = plane ? h / 2 : h;
        int samples = plane ? w : w; /* chroma: w/2 pairs = w samples */
        size_t base = plane ? (size_t)luma_rows * 128 : 0;
        for (int y = 0; y < rows; y++) {
            for (int n0 = 0; n0 < samples; n0 += 3) {
                uint32_t word = 0;
                for (int k = 0; k < 3 && n0 + k < samples; k++) {
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

/* A software-decoded picture as a DRM PRIME frame on a udmabuf, described as
 * the Pi decoder describes its own (one object, one layer, two planes). */
static AVFrame *to_drm(struct dv_source *s, AVFrame *sw)
{
    if (sw->format != AV_PIX_FMT_YUV420P10 && sw->format != AV_PIX_FMT_P010) {
        fprintf(stderr, "source: software decoder gave %s, need 10-bit 4:2:0\n", av_get_pix_fmt_name(sw->format));
        return NULL;
    }
    double c0 = cpu_now_ms();
    int w = sw->width, h = sw->height;
    AVDRMFrameDescriptor *desc = av_mallocz(sizeof(*desc));
    AVDRMLayerDescriptor *layer = &desc->layers[0];
    size_t size;
    uint64_t modifier;
    if (s->o.layout == DV_LAYOUT_SAND30) {
        uint32_t luma_rows = (uint32_t)align((size_t)h, 16), colh = luma_rows * 3 / 2;
        size_t columns = ((size_t)w + 95) / 96;
        size = columns * 128 * colh;
        modifier = DV_MOD_SAND128(colh);
        layer->format = DRM_FORMAT_P030;
        layer->planes[1].offset = (ptrdiff_t)luma_rows * 128;
        layer->planes[0].pitch = layer->planes[1].pitch = w * 2; /* as FFmpeg's V4L2 request code */
    } else {
        size_t pitch = align((size_t)w * 2, 256);
        size = pitch * (size_t)h * 3 / 2;
        modifier = DRM_FORMAT_MOD_LINEAR;
        layer->format = DRM_FORMAT_P010;
        layer->planes[1].offset = (ptrdiff_t)(pitch * (size_t)h);
        layer->planes[0].pitch = layer->planes[1].pitch = (ptrdiff_t)pitch;
    }
    size = align(size, 4096); /* dmabuf sizes are whole pages */
    struct udma *u = udma_get(s, size);
    if (!u) {
        av_free(desc);
        return NULL;
    }
    dv_dmabuf_cpu(&u->buf, 1, 1);
    if (s->o.layout == DV_LAYOUT_SAND30)
        write_sand30(sw, u->buf.map, dv_sand_height(modifier), (uint32_t)(layer->planes[1].offset / 128));
    else
        write_p010(sw, u->buf.map, (size_t)layer->planes[0].pitch, (size_t)layer->planes[1].offset);
    dv_dmabuf_cpu(&u->buf, 0, 1);
    desc->nb_objects = 1;
    desc->objects[0] = (AVDRMObjectDescriptor){.fd = u->buf.fd, .size = size, .format_modifier = modifier};
    desc->nb_layers = 1;
    layer->nb_planes = 2;

    AVFrame *f = av_frame_alloc();
    f->format = AV_PIX_FMT_DRM_PRIME;
    f->width = w;
    f->height = h;
    f->data[0] = (uint8_t *)desc;
    f->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc), udma_release, u, 0);
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
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, par) < 0)
        return NULL;
    if (hw) {
        c->hw_device_ctx = av_buffer_ref(s->device);
        c->get_format = pick_drm;
        c->extra_hw_frames = s->o.extra_frames;
    } else {
        c->thread_count = 0;
    }
    c->pkt_timebase = s->time_base;
    if (avcodec_open2(c, codec, NULL) < 0)
        return NULL;
    return c;
}

/* Single-track Profile 7: BL = all NALs but 63; EL = each NAL 63's payload. */
static int split_packet(const struct dv_source *s, const AVPacket *in, AVPacket *out, int el)
{
    size_t room = (size_t)in->size + 64 * 4;
    if (av_new_packet(out, (int)room) < 0)
        return AVERROR(ENOMEM);
    size_t n = 0;
    const uint8_t *q = in->data, *end = in->data + in->size;
    while (end - q > s->nal_size_len) {
        uint32_t len = 0;
        for (int i = 0; i < s->nal_size_len; i++)
            len = len << 8 | q[i];
        q += s->nal_size_len;
        if (len < 2 || len > (size_t)(end - q))
            break;
        int type = (q[0] >> 1) & 63;
        if (el && type == 63 && len > 2) {
            static const uint8_t start[4] = {0, 0, 0, 1};
            if (n + 4 + len - 2 > room)
                break;
            memcpy(out->data + n, start, 4);
            memcpy(out->data + n + 4, q + 2, len - 2);
            n += 4 + len - 2;
        } else if (!el && type != 63) {
            memcpy(out->data + n, q - s->nal_size_len, s->nal_size_len + len);
            n += s->nal_size_len + len;
        }
        q += len;
    }
    av_packet_copy_props(out, in);
    out->size = (int)n;
    return 0;
}

struct dv_source *dv_source_open(const char *path, const struct dv_source_options *o)
{
    struct dv_source *s = calloc(1, sizeof(*s));
    s->o = *o;
    s->nal_size_len = 4;
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
        s->nal_size_len = (par->extradata[21] & 3) + 1;
        AVCodecParameters *elp = avcodec_parameters_alloc();
        avcodec_parameters_copy(elp, par);
        av_freep(&elp->extradata);
        elp->extradata_size = 0;
        s->dec[1] = open_decoder(s, elp, hw && !o->el_software);
        avcodec_parameters_free(&elp);
        if (!s->dec[1])
            goto fail;
    }
    if (o->start > 0)
        av_seek_frame(s->fmt, s->stream, (int64_t)(o->start / av_q2d(s->time_base)), AVSEEK_FLAG_BACKWARD);
    return s;
fail:
    dv_source_close(s);
    return NULL;
}

int dv_source_profile(const struct dv_source *s) { return s->profile; }
double dv_source_copy_ms(const struct dv_source *s) { return s->copy_ms; }

AVRational dv_source_rate(const struct dv_source *s)
{
    return s->fmt->streams[s->stream]->avg_frame_rate;
}

static int drain_decoder(struct dv_source *s, int layer)
{
    for (;;) {
        if (s->queued[layer] >= QUEUE)
            return 0;
        AVFrame *f = av_frame_alloc();
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
        s->queue[layer][s->queued[layer]++] = f;
    }
}

static int feed(struct dv_source *s, int layer, const AVPacket *pkt)
{
    if (!s->dec[1]) {
        int r = avcodec_send_packet(s->dec[0], pkt);
        return r == AVERROR_EOF || r == AVERROR(EAGAIN) ? 0 : r;
    }
    if (!pkt) {
        avcodec_send_packet(s->dec[layer], NULL);
        return 0;
    }
    AVPacket *out = av_packet_alloc();
    int r = split_packet(s, pkt, out, layer);
    if (!r && out->size)
        r = avcodec_send_packet(s->dec[layer], out);
    av_packet_free(&out);
    return r == AVERROR(EAGAIN) ? 0 : r;
}

static AVFrame *pop(struct dv_source *s, int layer)
{
    AVFrame *f = s->queue[layer][0];
    memmove(s->queue[layer], s->queue[layer] + 1, sizeof(s->queue[0][0]) * (size_t)--s->queued[layer]);
    return f;
}

int dv_source_next(struct dv_source *s, struct dv_picture *p)
{
    memset(p, 0, sizeof(*p));
    for (;;) {
        int layers = s->dec[1] ? 2 : 1;
        if (layers == 2 && s->queued[1] == 0 && (s->queued[0] >= 8 || s->eof == 2)) {
            fprintf(stderr, "source: no enhancement-layer pictures; base layer only\n");
            avcodec_free_context(&s->dec[1]);
            layers = 1;
        }
        if (s->queued[0] > 0 && (layers == 1 || s->queued[1] > 0)) {
            p->bl = pop(s, 0);
            if (layers == 2)
                p->el = pop(s, 1);
            p->dovi = av_frame_get_side_data(p->bl, AV_FRAME_DATA_DOVI_METADATA);
            if (!p->dovi && p->el)
                p->dovi = av_frame_get_side_data(p->el, AV_FRAME_DATA_DOVI_METADATA);
            return 1;
        }
        if (s->eof == 2)
            return 0;
        if (s->eof) {
            s->eof = 2;
            for (int l = 0; l < layers; l++) {
                feed(s, l, NULL);
                drain_decoder(s, l);
            }
            continue;
        }
        AVPacket *pkt = av_packet_alloc();
        if (av_read_frame(s->fmt, pkt) < 0) {
            av_packet_free(&pkt);
            s->eof = 1;
            continue;
        }
        if (pkt->stream_index == s->stream) {
            for (int l = 0; l < layers; l++) {
                int r = feed(s, l, pkt);
                if (r >= 0)
                    r = drain_decoder(s, l);
                if (r < 0) {
                    fprintf(stderr, "source: layer %d: %s\n", l, av_err2str(r));
                    av_packet_free(&pkt);
                    return r;
                }
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
        avcodec_free_context(&s->dec[l]);
    }
    av_buffer_unref(&s->device);
    avformat_close_input(&s->fmt);
    for (int i = 0; i < POOL; i++)
        if (s->pool[i].buf.map)
            dv_dmabuf_free(&s->pool[i].buf);
    free(s);
}
