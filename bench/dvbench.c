/* dvbench: Dolby Vision decode and frame generation on the Raspberry Pi 5,
 * the Pi engine (engine/pi_engine.h) on decoded pictures. Headless: no
 * display, no audio.
 *
 *   dvbench [options] FILE
 *     --source sw|v4l2   decoder: FFmpeg software into SAND128 dmabufs laid
 *                        out as the Pi decoder's (any Linux with udmabuf or
 *                        a DMA heap; default) or the Pi's V4L2 request HEVC
 *     --el-sw            v4l2: decode the Profile 7 EL in software
 *     --frames N         pictures decoded and rendered (default 48; v4l2: 12)
 *     --warmup N         pictures left out of the per-picture timing (default 8)
 *     --start S          seek to S seconds first
 *     --fel 0|1          use the EL (default 1)
 *     --filters F        spatial filters: cb1 (default; CB1 Master Reference's
 *                        4:2:0 reshaping and Annex B 4:2:2 filters) or phase
 *                        (also `yblod`: YBLOD's chroma phase filters, 2-tap
 *                        4:2:2, the full-height single-layer MMR layout)
 *     --mmr M            chroma MMR: exact (default: float with an error bound,
 *                        exact where it could matter), split, exact64 (all
 *                        exact), float (within 2 codes)
 *     --device NAME      Vulkan device name substring (default: the first GPU)
 *     --pipeline-cache F the engine's pipeline cache file
 *     --throughput S     then flat out over the decoded pictures for S
 *                        seconds, three frames in flight
 *     --play S           then real-time playback for S seconds: a decoding
 *                        thread (8-picture queue) and rendering on the
 *                        stream's frame clock; counts pictures that missed
 *                        their display deadline and decode stalls
 *     --no-fences        --play: wait on the engine instead of its sync_files
 *     --selftest         only the API's behaviour on the decoded pictures: errors,
 *                        validation, frame lifetime, pipeline cache growth,
 *                        import release, teardown with renders in flight
 *     --kms-play S       then play S seconds on the HDMI output (KMS): the
 *                        stream's size and frame rate, RGB full range, 8 bits
 *                        per channel, each frame behind its sync_file
 *     --kms-writeback    then show the output on a KMS plane with the render's
 *                        sync_file as IN_FENCE_FD, captured by a writeback
 *                        connector (no display needed), and compare the
 *                        capture with the finished frame; again without the
 *                        fence, as the control
 *     --reopen           then close the stream, open it again in the same engine
 *                        and render the pictures again: identical output
 *                        (the planner's state reset, the imports released)
 *     --implicit-sync    per picture: does the output dmabuf carry an implicit
 *                        fence (what KMS waits on without a sync_file), busy
 *                        right after the render is submitted, idle after?
 *     --hash             print a hash of every output picture
 *     --dump DIR         write each output as DIR/pi-N.bgra
 *     --json FILE        machine-readable summary
 *     --measure LIST     measurement switches, comma-separated: all_rows,
 *                        draw_bars, piece_general, mark, copy_output,
 *                        force_wide (pi_options.measure; as the PI_* below)
 *     --probe            only check the dmabuf allocation, the device and an
 *                        output import (3840x2160); exit 0 if they work
 *   Environment: PI_DEBUG=1 the plan; PI_PROFILE=1 per-pass GPU times;
 *   PI_DEVICE (as --device). Measurement only (pi_options.measure): PI_ALL_ROWS,
 *   PI_DRAW_BARS, PI_PIECE_GENERAL, PI_MARK, PI_COPY_OUTPUT, PI_FORCE_WIDE,
 *   PI_ABLATE=N.
 *
 * Per picture the engine plans, imports (if new), renders and finishes
 * before the next starts: wall time from submit to completion. Outputs are
 * the HDMI tunnel's BGRA frame at the stream's size (12-bit 4:2:2 packed
 * with the metadata).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "source.h"
#include "alloc.h"
#include "pi_engine.h"
#include <errno.h>
#include <libavutil/dovi_meta.h>
#include <math.h>
#include <poll.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static double cpu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- output buffers: the engine's outputs on our own dmabufs ------------------ */

struct out_buf {
    struct dv_dmabuf buf;
    struct pi_output *out;
    uint32_t w, h, stride;
};

static int out_alloc(struct pi_engine *e, struct out_buf *o, uint32_t w, uint32_t h)
{
    uint64_t bytes;
    if (pi_output_size(w, h, &o->stride, &bytes) || dv_dmabuf_alloc(&o->buf, (size_t)bytes))
        return -1;
    o->w = w;
    o->h = h;
    int rc = pi_output_import(e, o->buf.fd, w, h, o->stride, o->buf.size, &o->out);
    if (rc) {
        fprintf(stderr, "pi: output import: %s\n", pi_strerror(rc));
        dv_dmabuf_free(&o->buf);
        return -1;
    }
    return 0;
}

static void out_free(struct pi_engine *e, struct out_buf *o)
{
    if (o->out)
        pi_output_release(e, o->out);
    if (o->buf.map)
        dv_dmabuf_free(&o->buf);
    memset(o, 0, sizeof(*o));
}

/* Over the picture's rows only (the buffer is larger). */
static uint64_t out_hash(const struct out_buf *o)
{
    dv_dmabuf_cpu(&o->buf, 1, 0);
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < o->h; y++) {
        const uint64_t *p = (const uint64_t *)(o->buf.map + (size_t)y * o->stride);
        for (uint32_t i = 0; i < o->w / 2; i++)
            h = (h ^ p[i]) * 0x100000001b3ull;
    }
    dv_dmabuf_cpu(&o->buf, 0, 0);
    return h;
}

static int out_dump(const struct out_buf *o, const char *dir, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/pi-%03d.bgra", dir, n);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "dvbench: %s: %s\n", path, strerror(errno));
        return -1;
    }
    dv_dmabuf_cpu(&o->buf, 1, 0);
    int ok = 1;
    for (uint32_t y = 0; y < o->h && ok; y++)
        ok = fwrite(o->buf.map + (size_t)y * o->stride, 4, o->w, f) == o->w;
    dv_dmabuf_cpu(&o->buf, 0, 0);
    ok &= fclose(f) == 0;
    if (!ok)
        fprintf(stderr, "dvbench: %s: write failed\n", path);
    return ok ? 0 : -1;
}

/* ---- statistics ------------------------------------------------------------ */

static int by_value(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

struct stats {
    double median, p95, mean;
    int n;
};

static struct stats summarize(double *t, int n)
{
    struct stats s = {0};
    if (n <= 0)
        return s;
    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += t[i];
    qsort(t, (size_t)n, sizeof(*t), by_value);
    s.median = t[n / 2];
    s.p95 = t[(int)(n * 0.95) < n ? (int)(n * 0.95) : n - 1];
    s.mean = sum / n;
    s.n = n;
    return s;
}

/* A JSON string: quotes, backslashes and control characters escaped. */
static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

/* ---- one picture ------------------------------------------------------------ */

static struct pi_engine *g_pi;
static int g_profile, g_fel = 1, g_fences = 1, g_implicit, g_reopen, g_kms, g_selftest;
static double g_kms_play;

/* The frame carrying the picture's RPU when it's the EL's (source.c). */
static const AVFrame *rpu_frame(const struct dv_picture *q)
{
    return q->el && !av_frame_get_side_data(q->bl, AV_FRAME_DATA_DOVI_METADATA) ? q->el : NULL;
}

/* acquire_fd: taken; release_fd: NULL, or the render's sync_file (-1 none). */
static int render_fenced(const struct dv_picture *q, double pts, struct out_buf *o, int acquire_fd, int *release_fd)
{
    struct pi_frame f = {.bl = q->bl, .el = g_fel ? q->el : NULL, .rpu_frame = rpu_frame(q),
                         .chroma_top_left = pi_chroma_top_left(q->bl, g_profile), .pts_us = (int64_t)(pts * 1e6),
                         .acquire_fd = acquire_fd};
    int rc = pi_render(g_pi, &f, o->out, release_fd);
    static int reported;
    if (rc && !reported++)
        fprintf(stderr, "pi: render %s: %s (%d)\n",
                rc == PI_E_UNSUPPORTED_PLAN || rc == PI_E_BAD_RPU ? "refused" : "failed", pi_strerror(rc), rc);
    return rc == 0;
}

static int render(const struct dv_picture *q, double pts, struct out_buf *o)
{
    return render_fenced(q, pts, o, -1, NULL);
}

/* A sync_file signalled (timeout 0: now, -1: waits). */
static int fence_done(int fd, int timeout)
{
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    int r;
    while ((r = poll(&pf, 1, timeout)) < 0 && errno == EINTR)
        ;
    return r > 0;
}

/* ---- real-time playback ------------------------------------------------------ */

/* --play: a decode thread fills a player-sized queue while the render loop
 * runs on the stream's frame clock: picture k is due on screen at tick k + 1
 * (one frame of latency, as a display flip). Counts pictures whose render
 * missed that deadline and ticks at which decoding hadn't delivered yet.
 * As a display would: each render's release sync_file is the deadline check,
 * and an output is rendered again only after the previous picture on it,
 * passed as the acquire fence (the scanout's, in KMS). */
#define PLAY_QUEUE 8

struct play_queue {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct dv_picture q[PLAY_QUEUE];
    int head, count, eof, stop;
    uint32_t w, h;
    struct dv_source *src;
};

static void *play_decoder(void *arg)
{
    struct play_queue *pq = arg;
    for (;;) {
        struct dv_picture p;
        int r = dv_source_next(pq->src, &p);
        if (r > 0 && (!p.dovi || (uint32_t)p.bl->width != pq->w || (uint32_t)p.bl->height != pq->h)) {
            dv_picture_free(&p);
            continue;
        }
        pthread_mutex_lock(&pq->lock);
        while (r > 0 && pq->count == PLAY_QUEUE && !pq->stop)
            pthread_cond_wait(&pq->cond, &pq->lock);
        if (r <= 0 || pq->stop) {
            pq->eof = 1;
            pthread_cond_broadcast(&pq->cond);
            pthread_mutex_unlock(&pq->lock);
            if (r > 0)
                dv_picture_free(&p);
            return NULL;
        }
        pq->q[(pq->head + pq->count++) % PLAY_QUEUE] = p;
        pthread_cond_broadcast(&pq->cond);
        pthread_mutex_unlock(&pq->lock);
    }
}

static void sleep_until_ms(double t)
{
    double d = t - now_ms();
    if (d > 0) {
        struct timespec ts = {(time_t)(d / 1e3), (long)(fmod(d, 1e3) * 1e6)};
        nanosleep(&ts, NULL);
    }
}

struct play_result {
    long shown, late, starved, failed;
    int fenced;   /* the engine exported release fences */
    double worst_late_ms, cpu_ms, wall_ms;
};

static int play(const char *file, struct dv_source_options so, double seconds, struct out_buf *out,
                struct play_result *res)
{
    memset(res, 0, sizeof(*res));
    so.extra_frames = PLAY_QUEUE + 6;
    struct play_queue pq = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                            .w = out[0].w, .h = out[0].h};
    pq.src = dv_source_open(file, &so);
    if (!pq.src)
        return -1;
    AVRational rate = dv_source_rate(pq.src);
    double period = rate.num > 0 ? 1e3 * rate.den / rate.num : 1e3 / 24;
    pthread_t thread;
    if (pthread_create(&thread, NULL, play_decoder, &pq)) {
        dv_source_close(pq.src);
        return -1;
    }
    /* Start once half the queue is buffered, as a player prerolls. */
    pthread_mutex_lock(&pq.lock);
    while (pq.count < PLAY_QUEUE / 2 && !pq.eof)
        pthread_cond_wait(&pq.cond, &pq.lock);
    pthread_mutex_unlock(&pq.lock);

    /* In flight: the picture each output shows, and its deadline. */
    struct dv_picture held[3] = {{0}};
    int pending[3] = {0}, fence[3] = {-1, -1, -1};
    double due[3] = {0};
    double t0 = now_ms(), c0 = cpu_ms();
    long frames = (long)(seconds * 1e3 / period);
    for (long k = 0; k < frames; k++) {
        double tick = t0 + k * period;
        sleep_until_ms(tick);
        unsigned slot = (unsigned)(k % 3);
        /* The picture rendered three ticks ago must be done before its output
         * is reused; whether it met its own deadline was judged then. */
        int acquire = -1;
        if (pending[slot]) {
            acquire = fence[slot];   /* the output's last picture: done before it is drawn over */
            fence[slot] = -1;
            pi_output_wait(g_pi, out[slot].out);
            dv_picture_free(&held[slot]);
            pending[slot] = 0;
        }
        /* Deadline check of the previous picture: due at this tick. */
        unsigned prev = (unsigned)((k + 2) % 3);
        if (k > 0 && pending[prev] &&
            !(fence[prev] >= 0 ? fence_done(fence[prev], 0) : pi_output_done(g_pi, out[prev].out))) {
            res->late++;
            if (fence[prev] >= 0)
                fence_done(fence[prev], -1);
            pi_output_wait(g_pi, out[prev].out);
            double l = now_ms() - due[prev];
            if (l > res->worst_late_ms)
                res->worst_late_ms = l;
        }
        /* The next decoded picture. */
        pthread_mutex_lock(&pq.lock);
        if (!pq.count && !pq.eof)
            res->starved++;
        while (!pq.count && !pq.eof)
            pthread_cond_wait(&pq.cond, &pq.lock);
        if (!pq.count) {
            pthread_mutex_unlock(&pq.lock);
            if (acquire >= 0)
                close(acquire);
            break;
        }
        struct dv_picture p = pq.q[pq.head];
        pq.head = (pq.head + 1) % PLAY_QUEUE;
        pq.count--;
        pthread_cond_broadcast(&pq.cond);
        pthread_mutex_unlock(&pq.lock);

        if (!render_fenced(&p, k * period / 1e3, &out[slot], acquire, g_fences ? &fence[slot] : NULL)) {
            res->failed++;
            dv_picture_free(&p);
            continue;
        }
        held[slot] = p;
        pending[slot] = 1;
        due[slot] = tick + period;
        res->shown++;
    }
    for (unsigned s = 0; s < 3; s++) {
        if (pending[s]) {
            pi_output_wait(g_pi, out[s].out);
            dv_picture_free(&held[s]);
        }
        if (fence[s] >= 0) {
            res->fenced = 1;
            close(fence[s]);
        }
    }
    res->wall_ms = now_ms() - t0;
    res->cpu_ms = cpu_ms() - c0;
    pthread_mutex_lock(&pq.lock);
    pq.stop = 1;
    pthread_cond_broadcast(&pq.cond);
    pthread_mutex_unlock(&pq.lock);
    pthread_join(thread, NULL);
    while (pq.count) {
        dv_picture_free(&pq.q[pq.head]);
        pq.head = (pq.head + 1) % PLAY_QUEUE;
        pq.count--;
    }
    dv_source_close(pq.src);
    return 0;
}

/* ---- options ----------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr, "usage: dvbench [--source sw|v4l2] [--el-sw] [--frames N] [--warmup N] [--start S] [--fel 0|1]\n"
                    "               [--filters cb1|phase] [--mmr exact|split|exact64|float] [--device NAME]\n"
                    "               [--pipeline-cache FILE] [--throughput S] [--play S] [--hash] [--dump DIR]\n"
                    "               [--json FILE] [--measure LIST] FILE\n"
                    "       dvbench --probe [--device NAME]\n");
}

static int bad_value(const char *opt, const char *v)
{
    fprintf(stderr, "dvbench: %s: invalid value '%s'\n", opt, v);
    usage();
    return 0;
}

static int to_int(const char *opt, const char *v, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long n = strtol(v, &end, 10);
    if (errno || end == v || *end || n < lo || n > hi)
        return bad_value(opt, v);
    *out = (int)n;
    return 1;
}

static int to_seconds(const char *opt, const char *v, double *out)
{
    char *end;
    errno = 0;
    double d = strtod(v, &end);
    if (errno || end == v || *end || !(d >= 0) || d > 1e6)
        return bad_value(opt, v);
    *out = d;
    return 1;
}

static const struct {
    const char *name, *env;
    unsigned flag;
} knobs[] = {{"all_rows", "PI_ALL_ROWS", PI_MEASURE_ALL_ROWS},
             {"draw_bars", "PI_DRAW_BARS", PI_MEASURE_DRAW_BARS},
             {"piece_general", "PI_PIECE_GENERAL", PI_MEASURE_PIECE_GENERAL},
             {"mark", "PI_MARK", PI_MEASURE_MARK},
             {"copy_output", "PI_COPY_OUTPUT", PI_MEASURE_COPY_OUTPUT},
             {"force_wide", "PI_FORCE_WIDE", PI_MEASURE_FORCE_WIDE}};
enum { NKNOBS = sizeof(knobs) / sizeof(knobs[0]) };

static int measure_list(const char *opt, const char *v, unsigned *out)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", v);
    for (char *save, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        int k = 0;
        while (k < NKNOBS && strcmp(knobs[k].name, t))
            k++;
        if (k == NKNOBS)
            return bad_value(opt, t);
        *out |= knobs[k].flag;
    }
    return 1;
}

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0;
}

/* ---- KMS: the output on a plane, the release sync_file as IN_FENCE_FD ------ */

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; p && i < p->count_props && !id; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, name))
            id = r->prop_id;
        drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return id;
}

struct kms {
    int fd;
    uint32_t crtc, conn, plane, mode_blob, fb_in, fb_wb, wb_handle, wb_pitch;
    uint8_t *wb_map;
    size_t wb_size;
};

/* A writeback connector, a CRTC and a plane for it, the output as a
 * framebuffer and a dumb buffer to capture into. 0 OK. */
static int kms_open(struct kms *k, const struct out_buf *o)
{
    memset(k, 0, sizeof(*k));
    for (int card = 0; card < 4; card++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", card);
        k->fd = open(path, O_RDWR | O_CLOEXEC);
        if (k->fd < 0)
            continue;
        drmModeRes *res = NULL;
        if (!drmSetClientCap(k->fd, DRM_CLIENT_CAP_ATOMIC, 1) &&
            !drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) &&
            !drmSetClientCap(k->fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1))
            res = drmModeGetResources(k->fd);
        for (int i = 0; res && i < res->count_connectors && !k->conn; i++) {
            drmModeConnector *c = drmModeGetConnector(k->fd, res->connectors[i]);
            if (c && c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK && c->count_encoders) {
                drmModeEncoder *enc = drmModeGetEncoder(k->fd, c->encoders[0]);
                int ci = enc ? __builtin_ffs((int)enc->possible_crtcs) - 1 : -1;
                /* the stream's size among the connector's modes, else a CEA-style
                 * mode of that size (writeback needs no real timings) */
                drmModeModeInfo own = {0};
                if (c->count_modes) {
                    own = c->modes[0];
                    own.hdisplay = (uint16_t)o->w;
                    own.hsync_start = (uint16_t)(o->w + 176);
                    own.hsync_end = (uint16_t)(o->w + 264);
                    own.htotal = (uint16_t)(o->w + 560);
                    own.vdisplay = (uint16_t)o->h;
                    own.vsync_start = (uint16_t)(o->h + 8);
                    own.vsync_end = (uint16_t)(o->h + 18);
                    own.vtotal = (uint16_t)(o->h + 90);
                    own.vrefresh = 30;
                    own.clock = (uint32_t)((uint64_t)own.htotal * own.vtotal * 30 / 1000);
                    own.type = DRM_MODE_TYPE_USERDEF;
                    snprintf(own.name, sizeof(own.name), "%ux%u", o->w, o->h);
                }
                for (int m = 0; ci >= 0 && ci < res->count_crtcs && m <= c->count_modes && !k->conn; m++)
                    if ((m == c->count_modes ? c->count_modes > 0
                                             : c->modes[m].hdisplay == o->w && c->modes[m].vdisplay == o->h) &&
                        !drmModeCreatePropertyBlob(k->fd, m == c->count_modes ? &own : &c->modes[m], sizeof(own),
                                                   &k->mode_blob)) {
                        k->conn = c->connector_id;
                        k->crtc = res->crtcs[ci];
                        drmModePlaneRes *pr = drmModeGetPlaneResources(k->fd);
                        for (uint32_t p = 0; pr && p < pr->count_planes && !k->plane; p++) {
                            drmModePlane *pl = drmModeGetPlane(k->fd, pr->planes[p]);
                            for (uint32_t f = 0; pl && (pl->possible_crtcs & (1u << ci)) && f < pl->count_formats; f++)
                                if (pl->formats[f] == DRM_FORMAT_XRGB8888)
                                    k->plane = pl->plane_id;
                            drmModeFreePlane(pl);
                        }
                        drmModeFreePlaneResources(pr);
                    }
                drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
        if (k->conn && k->plane)
            break;
        close(k->fd);
        k->fd = -1;
        k->conn = 0;
    }
    if (k->fd < 0) {
        fprintf(stderr, "kms: no writeback connector with a %ux%u mode and an XRGB8888 plane\n", o->w, o->h);
        return -1;
    }
    uint32_t h, handles[4] = {0}, pitches[4] = {o->stride}, offsets[4] = {0};
    if (drmPrimeFDToHandle(k->fd, o->buf.fd, &h))
        return -1;
    handles[0] = h;
    if (drmModeAddFB2(k->fd, o->w, o->h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &k->fb_in, 0))
        return -1;
    struct drm_mode_create_dumb cd = {.width = o->w, .height = o->h, .bpp = 32};
    if (drmIoctl(k->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
        return -1;
    k->wb_handle = cd.handle;
    k->wb_pitch = cd.pitch;
    k->wb_size = cd.size;
    handles[0] = cd.handle;
    pitches[0] = cd.pitch;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (drmModeAddFB2(k->fd, o->w, o->h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &k->fb_wb, 0) ||
        drmIoctl(k->fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
        return -1;
    k->wb_map = mmap(NULL, k->wb_size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, (off_t)md.offset);
    return k->wb_map == MAP_FAILED ? -1 : 0;
}

/* One commit: the output on the plane (IN_FENCE_FD = in_fence unless -1),
 * captured. Blocking: KMS waits for the plane's fence before it reads. */
static int kms_commit(struct kms *k, const struct out_buf *o, int in_fence)
{
    int32_t wb_fence = -1;
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    uint32_t P = DRM_MODE_OBJECT_PLANE, C = DRM_MODE_OBJECT_CRTC, N = DRM_MODE_OBJECT_CONNECTOR;
    drmModeAtomicAddProperty(r, k->crtc, prop_id(k->fd, k->crtc, C, "MODE_ID"), k->mode_blob);
    drmModeAtomicAddProperty(r, k->crtc, prop_id(k->fd, k->crtc, C, "ACTIVE"), 1);
    drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, N, "CRTC_ID"), k->crtc);
    drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, N, "WRITEBACK_FB_ID"), k->fb_wb);
    drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, N, "WRITEBACK_OUT_FENCE_PTR"),
                             (uint64_t)(uintptr_t)&wb_fence);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "FB_ID"), k->fb_in);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_ID"), k->crtc);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_X"), 0);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_Y"), 0);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_W"), (uint64_t)o->w << 16);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_H"), (uint64_t)o->h << 16);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_X"), 0);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_Y"), 0);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_W"), o->w);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_H"), o->h);
    if (in_fence >= 0)
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "IN_FENCE_FD"), (uint64_t)in_fence);
    int rc = drmModeAtomicCommit(k->fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(r);
    if (rc) {
        fprintf(stderr, "kms: atomic commit failed: %s\n", strerror(errno));
        return -1;
    }
    if (wb_fence >= 0) {   /* the capture written */
        fence_done(wb_fence, 1000);
        close(wb_fence);
    }
    return 0;
}

static void kms_close(struct kms *k)
{
    if (k->fd < 0)
        return;
    drmModeAtomicReq *r = drmModeAtomicAlloc();   /* everything off again */
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "FB_ID"), 0);
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), 0);
    drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), 0);
    drmModeAtomicAddProperty(r, k->crtc, prop_id(k->fd, k->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 0);
    drmModeAtomicAddProperty(r, k->crtc, prop_id(k->fd, k->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), 0);
    drmModeAtomicCommit(k->fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(r);
    if (k->wb_map && k->wb_map != MAP_FAILED)
        munmap(k->wb_map, k->wb_size);
    if (k->fb_wb)
        drmModeRmFB(k->fd, k->fb_wb);
    if (k->fb_in)
        drmModeRmFB(k->fd, k->fb_in);
    if (k->wb_handle) {
        struct drm_mode_destroy_dumb dd = {.handle = k->wb_handle};
        drmIoctl(k->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    if (k->mode_blob)
        drmModeDestroyPropertyBlob(k->fd, k->mode_blob);
    close(k->fd);
}

/* ---- KMS: playing on the HDMI output ------------------------------------------- */

struct kms_out {
    int fd;
    uint32_t crtc, conn, plane, mode_blob, fb[3];
};

/* The connected HDMI connector, its CRTC and primary plane, a mode of the
 * stream's size nearest its frame rate, and the outputs as framebuffers. */
static int kms_out_open(struct kms_out *k, const struct out_buf *o, double rate)
{
    memset(k, 0, sizeof(*k));
    k->fd = -1;
    for (int card = 0; card < 4 && !k->conn; card++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", card);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmModeRes *res = NULL;
        if (!drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) && !drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
            res = drmModeGetResources(fd);
        for (int i = 0; res && i < res->count_connectors && !k->conn; i++) {
            drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
            if (c && c->connection == DRM_MODE_CONNECTED && c->connector_type == DRM_MODE_CONNECTOR_HDMIA &&
                c->count_encoders) {
                drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoders[0]);
                int ci = enc ? __builtin_ffs((int)enc->possible_crtcs) - 1 : -1;
                int best = -1;
                double best_err = 1e9;
                for (int m = 0; ci >= 0 && m < c->count_modes; m++) {
                    const drmModeModeInfo *mi = &c->modes[m];
                    if (mi->hdisplay != o->w || mi->vdisplay != o->h || (mi->flags & DRM_MODE_FLAG_INTERLACE))
                        continue;
                    double hz = mi->clock * 1000.0 / ((double)mi->htotal * mi->vtotal);
                    if (fabs(hz - rate) < best_err)
                        best_err = fabs(hz - rate), best = m;
                }
                if (best >= 0 && !drmModeCreatePropertyBlob(fd, &c->modes[best], sizeof(c->modes[best]), &k->mode_blob)) {
                    const drmModeModeInfo *mi = &c->modes[best];
                    printf("kms: %s on card%d: %ux%u at %.3f Hz, RGB full range, 8 bpc\n", "HDMI", card, mi->hdisplay,
                           mi->vdisplay, mi->clock * 1000.0 / ((double)mi->htotal * mi->vtotal));
                    k->fd = fd;
                    k->conn = c->connector_id;
                    k->crtc = res->crtcs[ci];
                    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
                    for (uint32_t p = 0; pr && p < pr->count_planes && !k->plane; p++) {
                        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[p]);
                        drmModeObjectProperties *pp = pl ? drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE) : NULL;
                        for (uint32_t q = 0; pp && (pl->possible_crtcs & (1u << ci)) && q < pp->count_props; q++) {
                            drmModePropertyRes *r = drmModeGetProperty(fd, pp->props[q]);
                            if (r && !strcmp(r->name, "type") && pp->prop_values[q] == DRM_PLANE_TYPE_PRIMARY)
                                k->plane = pl->plane_id;
                            drmModeFreeProperty(r);
                        }
                        drmModeFreeObjectProperties(pp);
                        drmModeFreePlane(pl);
                    }
                    drmModeFreePlaneResources(pr);
                }
                drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
        if (!k->conn)
            close(fd);
    }
    if (!k->conn || !k->plane) {
        fprintf(stderr, "kms: no connected HDMI output with a %ux%u mode\n", o->w, o->h);
        return -1;
    }
    for (int i = 0; i < 3; i++) {
        uint32_t h, handles[4] = {0}, pitches[4] = {o[i].stride}, offsets[4] = {0};
        if (drmPrimeFDToHandle(k->fd, o[i].buf.fd, &h))
            return -1;
        handles[0] = h;
        if (drmModeAddFB2(k->fd, o[i].w, o[i].h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &k->fb[i], 0))
            return -1;
    }
    return 0;
}

static uint64_t enum_value(int fd, uint32_t obj, uint32_t type, const char *prop, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    uint64_t v = 0;
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *r = drmModeGetProperty(fd, p->props[i]);
        if (r && !strcmp(r->name, prop))
            for (int e = 0; e < r->count_enums; e++)
                if (!strcmp(r->enums[e].name, name))
                    v = r->enums[e].value;
        drmModeFreeProperty(r);
    }
    drmModeFreeObjectProperties(p);
    return v;
}

/* Output i on the plane, behind fence (-1 none); blocking: returns once shown. */
static int kms_out_show(struct kms_out *k, const struct out_buf *o, int i, int fence, int first)
{
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    uint32_t P = DRM_MODE_OBJECT_PLANE, C = DRM_MODE_OBJECT_CRTC, N = DRM_MODE_OBJECT_CONNECTOR;
    if (first) {
        drmModeAtomicAddProperty(r, k->crtc, prop_id(k->fd, k->crtc, C, "MODE_ID"), k->mode_blob);
        drmModeAtomicAddProperty(r, k->crtc, prop_id(k->fd, k->crtc, C, "ACTIVE"), 1);
        drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, N, "CRTC_ID"), k->crtc);
        drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, N, "Broadcast RGB"),
                                 enum_value(k->fd, k->conn, N, "Broadcast RGB", "Full"));
        drmModeAtomicAddProperty(r, k->conn, prop_id(k->fd, k->conn, N, "max bpc"), 8);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_ID"), k->crtc);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_X"), 0);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_Y"), 0);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_W"), (uint64_t)o->w << 16);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "SRC_H"), (uint64_t)o->h << 16);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_X"), 0);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_Y"), 0);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_W"), o->w);
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "CRTC_H"), o->h);
    }
    drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "FB_ID"), k->fb[i]);
    if (fence >= 0)
        drmModeAtomicAddProperty(r, k->plane, prop_id(k->fd, k->plane, P, "IN_FENCE_FD"), (uint64_t)fence);
    int rc = drmModeAtomicCommit(k->fd, r, first ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, NULL);
    drmModeAtomicFree(r);
    if (rc)
        fprintf(stderr, "kms: commit failed: %s\n", strerror(errno));
    return rc;
}

static void kms_out_close(struct kms_out *k)
{
    if (k->fd < 0)
        return;
    for (int i = 0; i < 3; i++)
        if (k->fb[i])
            drmModeRmFB(k->fd, k->fb[i]);
    if (k->mode_blob)
        drmModeDestroyPropertyBlob(k->fd, k->mode_blob);
    close(k->fd);   /* the console takes the output back */
}

/* --kms-play: the decoding thread of --play, each picture rendered and shown
 * on the HDMI output behind its sync_file, paced by the display. */
static int kms_play(const char *file, struct dv_source_options so, double seconds, struct out_buf *out)
{
    so.extra_frames = PLAY_QUEUE + 6;
    struct play_queue pq = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
                            .w = out[0].w, .h = out[0].h};
    pq.src = dv_source_open(file, &so);
    if (!pq.src)
        return -1;
    AVRational rate = dv_source_rate(pq.src);
    double fps = rate.num > 0 ? (double)rate.num / rate.den : 24;
    struct kms_out k;
    if (kms_out_open(&k, out, fps)) {
        kms_out_close(&k);
        dv_source_close(pq.src);
        return -1;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, play_decoder, &pq)) {
        kms_out_close(&k);
        dv_source_close(pq.src);
        return -1;
    }
    struct dv_picture held[3] = {{0}};
    long shown = 0, failed = 0, frames = (long)(seconds * fps);
    double t0 = now_ms();
    for (long n = 0; n < frames; n++) {
        pthread_mutex_lock(&pq.lock);
        while (!pq.count && !pq.eof)
            pthread_cond_wait(&pq.cond, &pq.lock);
        if (!pq.count) {
            pthread_mutex_unlock(&pq.lock);
            break;
        }
        struct dv_picture p = pq.q[pq.head];
        pq.head = (pq.head + 1) % PLAY_QUEUE;
        pq.count--;
        pthread_cond_broadcast(&pq.cond);
        pthread_mutex_unlock(&pq.lock);
        int slot = (int)(n % 3), fence = -1;
        /* the output shown three frames ago is free: two flips have passed */
        pi_output_wait(g_pi, out[slot].out);
        dv_picture_free(&held[slot]);
        if (!render_fenced(&p, n / fps, &out[slot], -1, &fence)) {
            failed++;
            dv_picture_free(&p);
            continue;
        }
        held[slot] = p;
        int rc = kms_out_show(&k, &out[slot], slot, fence, shown == 0);
        if (fence >= 0)
            close(fence);
        if (rc)
            break;
        shown++;
    }
    double wall = now_ms() - t0;
    printf("kms-play: %ld pictures shown in %.1f s (%.2f fps; the stream's %.3f)%s\n", shown, wall / 1e3,
           shown * 1e3 / wall, fps, failed ? " FAILURES" : "");
    for (int s = 0; s < 3; s++) {
        pi_output_wait(g_pi, out[s].out);
        dv_picture_free(&held[s]);
    }
    pthread_mutex_lock(&pq.lock);
    pq.stop = 1;
    pthread_cond_broadcast(&pq.cond);
    pthread_mutex_unlock(&pq.lock);
    pthread_join(thread, NULL);
    while (pq.count) {
        dv_picture_free(&pq.q[pq.head]);
        pq.head = (pq.head + 1) % PLAY_QUEUE;
        pq.count--;
    }
    kms_out_close(&k);
    dv_source_close(pq.src);
    return failed ? 1 : 0;
}

/* Per picture: the output filled with a marker, rendered, committed at once
 * (fenced or not), captured; then the capture against the finished frame.
 * Returns the pictures whose capture was the finished frame. */
static int kms_run(struct kms *k, const struct dv_picture *pics, int count, double period, struct out_buf *o,
                   int fenced, int *marked)
{
    int match = 0;
    *marked = 0;
    for (int i = 0; i < count; i++) {
        dv_dmabuf_cpu(&o->buf, 1, 1);
        memset(o->buf.map, 0xa5, o->buf.size);
        dv_dmabuf_cpu(&o->buf, 0, 1);
        int fence = -1;
        if (!render_fenced(&pics[i], i * period, o, -1, fenced ? &fence : NULL))
            continue;
        if (fenced && fence < 0) {
            fprintf(stderr, "kms: no release sync_file from the engine\n");
            return -1;
        }
        int busy = !pi_output_done(g_pi, o->out);
        double t0 = now_ms();
        int rc = kms_commit(k, o, fence);
        double t1 = now_ms() - t0;
        if (getenv("DVBENCH_KMS_DEBUG"))
            fprintf(stderr, "kms: %s commit: GPU %s at commit, commit + capture %.1f ms, GPU %s after\n",
                    fenced ? "fenced" : "bare", busy ? "busy" : "done", t1,
                    pi_output_done(g_pi, o->out) ? "done" : "busy");
        if (fence >= 0)
            close(fence);
        pi_output_wait(g_pi, o->out);
        if (rc)
            return -1;
        dv_dmabuf_cpu(&o->buf, 1, 0);
        int same = 1, mark = 0;
        for (uint32_t y = 0; y < o->h; y++) {
            const uint32_t *a = (const uint32_t *)(o->buf.map + (size_t)y * o->stride);
            const uint32_t *b = (const uint32_t *)(k->wb_map + (size_t)y * k->wb_pitch);
            for (uint32_t x = 0; x < o->w; x++) {
                same &= ((a[x] ^ b[x]) & 0xffffffu) == 0;
                mark |= (b[x] & 0xffffffu) == 0xa5a5a5u;
            }
        }
        dv_dmabuf_cpu(&o->buf, 0, 0);
        match += same;
        *marked += mark;
    }
    return match;
}

/* ---- --selftest ---------------------------------------------------------------- */

struct logcount {
    int variants, closed, released, mmr;
};

static void count_log(void *opaque, enum pi_log_level level, const char *msg)
{
    struct logcount *c = opaque;
    int n;
    if (strstr(msg, "pipeline P1 variant made"))
        c->variants++;
    if (strstr(msg, "one MMR piece") || (strstr(msg, "tables:") && strstr(msg, "route 1") && !strstr(msg, "kinds 0 0")))
        c->mmr = 1;   /* the streams whose MMR variants are built ahead */
    if (sscanf(msg, "stream closed: %d imports released", &n) == 1) {
        c->closed++;
        c->released = n;
    }
    if (level <= PI_LOG_WARNING)
        fprintf(stderr, "pi: %s\n", msg);
}

static int st_fails, st_runs;

static void st_check(int ok, const char *what, int got)
{
    st_runs++;
    st_fails += !ok;
    printf("selftest: %s  %s (%d)\n", ok ? "ok  " : "FAIL", what, got);
}

/* A frame for the pairer's checks: its pts, and an RPU with a residual (1),
 * without one (0), or none (-1). */
static AVFrame *pair_frame(int64_t pts, int residual)
{
    AVFrame *f = av_frame_alloc();
    if (!f)
        return NULL;
    f->pts = pts;
    if (residual >= 0) {
        size_t size;
        AVDOVIMetadata *m = av_dovi_metadata_alloc(&size);
        AVBufferRef *b = m ? av_buffer_create((uint8_t *)m, size, NULL, NULL, 0) : NULL;
        if (!b) {
            av_free(m);
            av_frame_free(&f);
            return NULL;
        }
        av_dovi_get_header(m)->disable_residual_flag = !residual;
        if (!av_frame_new_side_data_from_buf(f, AV_FRAME_DATA_DOVI_METADATA, b)) {
            av_buffer_unref(&b);
            av_frame_free(&f);
        }
    }
    return f;
}

/* Pops every pair (end), each as "bl/el" pts ("-" none), into out. */
static void pair_drain(struct pi_pairer *pp, int end, char *out, size_t room)
{
    AVFrame *bl, *el;
    out[0] = 0;
    while (pi_pairer_pop(pp, end, &bl, &el) == 1) {
        size_t n = strlen(out);
        if (el)
            snprintf(out + n, room - n, "%s%lld/%lld", n ? " " : "", (long long)bl->pts, (long long)el->pts);
        else
            snprintf(out + n, room - n, "%s%lld/-", n ? " " : "", (long long)bl->pts);
        av_frame_free(&bl);
        av_frame_free(&el);
    }
}

/* The BL / EL pairer and pi_frame_needs_el, on synthetic frames. */
static void pair_selftest(void)
{
    AVFrame *a = pair_frame(0, 1), *b = pair_frame(0, 0), *c = pair_frame(0, -1);
    st_check(pi_frame_needs_el(a) == 1 && pi_frame_needs_el(b) == 0 && pi_frame_needs_el(c) == PI_E_BAD_RPU,
             "pi_frame_needs_el: a residual 1, none 0, no RPU PI_E_BAD_RPU", pi_frame_needs_el(a));
    av_frame_free(&a);
    av_frame_free(&b);
    av_frame_free(&c);

    struct pi_pairer *pp;
    char got[256];
    int ok = !pi_pairer_create(2, &pp);
    /* the EL decoder's output, in display order, carries its packets' times */
    static const int64_t el_pts[8] = {0, 3, 1, 2, 6, 4, 5, 7};
    for (int i = 0; ok && i < 8; i++)
        ok = !pi_pairer_push(pp, 0, pair_frame(i, 1), 0) && !pi_pairer_push(pp, 1, pair_frame(el_pts[i], -1), 2);
    pair_drain(pp, 1, got, sizeof(got));
    st_check(ok && !strcmp(got, "0/0 1/1 2/2 3/3 4/4 5/5 6/6 7/7"),
             "pairer: an EL in another coding order pairs by display time", ok);
    if (strcmp(got, "0/0 1/1 2/2 3/3 4/4 5/5 6/6 7/7"))
        printf("selftest: pairs %s\n", got);
    pi_pairer_destroy(pp);

    /* lost: EL 2 (BL needs it), EL 3 (BL doesn't: MEL), BL 4 */
    ok = !pi_pairer_create(2, &pp);
    static const int64_t bls[5] = {0, 1, 2, 3, 5}, els[4] = {0, 1, 4, 5};
    for (int i = 0; ok && i < 5; i++)
        ok = !pi_pairer_push(pp, 0, pair_frame(bls[i], bls[i] != 3), 0);
    for (int i = 0; ok && i < 4; i++)
        ok = !pi_pairer_push(pp, 1, pair_frame(els[i], -1), 0);
    pair_drain(pp, 1, got, sizeof(got));
    struct pi_pair_stats st;
    pi_pairer_stats(pp, &st);
    int lost = ok && !strcmp(got, "0/0 1/1 3/- 5/5") && st.bl_dropped == 1 && st.bl_alone == 1 && st.el_dropped == 1;
    st_check(lost, "pairer: a lost EL drops its FEL BL, gives a MEL BL alone; a lost BL drops its EL", ok);
    if (!lost)
        printf("selftest: pairs %s (BLs dropped %u, alone %u, ELs dropped %u)\n", got, st.bl_dropped, st.bl_alone,
               st.el_dropped);
    pi_pairer_destroy(pp);

    /* an EL decoder that gives nothing */
    ok = !pi_pairer_create(2, &pp);
    for (int i = 0; ok && i < 8; i++)
        ok = !pi_pairer_push(pp, 0, pair_frame(i, 1), 0);
    pair_drain(pp, 0, got, sizeof(got));
    pi_pairer_stats(pp, &st);
    st_check(ok && st.el_absent && !strcmp(got, "0/- 1/- 2/- 3/- 4/- 5/- 6/- 7/-"),
             "pairer: no EL pictures by the 8th BL: BLs alone", st.el_absent);
    pi_pairer_destroy(pp);
}

static int selftest(struct pi_options base, struct dv_picture *pics, int count, uint32_t W, uint32_t H, double period)
{
    struct logcount lc = {0};
    struct pi_options po = base;
    po.log = count_log;
    po.log_opaque = &lc;
    po.log_level = PI_LOG_DEBUG;
    struct pi_engine *e;
    if (pi_engine_create(&po, &e))
        return 1;
    g_pi = e;
    pair_selftest();
    struct pi_probe_info info;
    const AVFrame *rf = pics[0].el && !av_frame_get_side_data(pics[0].bl, AV_FRAME_DATA_DOVI_METADATA) ? pics[0].el
                                                                                                      : pics[0].bl;
    int needs = pi_frame_needs_el(rf);
    st_check(pics[0].el ? needs == 0 || needs == 1 : needs == 0,
             "pi_frame_needs_el on the clip: 0 or 1, 0 without an EL", needs);
    int probed = pi_probe(&po, &info);
    st_check(!probed && !strncmp(pi_engine_describe(e), info.device, strlen(info.device)),
             "pi_probe: the engine's device", probed);
    struct out_buf o[2];
    memset(o, 0, sizeof(o));
    struct pi_stream_info si = {.width = W, .height = H, .profile = g_profile};
    struct pi_frame f = {.acquire_fd = -1};
    int rc;

    /* call order and arguments */
    st_check(pi_render(e, NULL, NULL, NULL) == PI_E_INVALID, "render with no frame: PI_E_INVALID", 0);
    if (out_alloc(e, &o[0], W, H) || out_alloc(e, &o[1], W, H))
        return 1;
    rc = render(&pics[0], 0, &o[0]);
    st_check(!rc, "render before pi_stream_open refused", rc);
    st_check(pi_stream_open(e, &(struct pi_stream_info){.width = 15, .height = 8}) == PI_E_INVALID,
             "odd stream size: PI_E_INVALID", 0);
    st_check(pi_stream_open(e, &si) == 0, "stream open", 0);

    /* output validation */
    struct pi_output *bad = NULL;
    st_check(pi_output_import(e, o[1].buf.fd, W, H, W * 4 - 4, o[1].buf.size, &bad) == PI_E_INVALID,
             "output stride below width x 4: PI_E_INVALID", 0);
    st_check(pi_output_import(e, o[1].buf.fd, W, H, o[1].stride, (uint64_t)o[1].stride * H, &bad) == PI_E_INVALID,
             "output a row short: PI_E_INVALID", 0);
    off_t real = lseek(o[1].buf.fd, 0, SEEK_END);   /* the kernel's size: its page size may exceed ours */
    lseek(o[1].buf.fd, 0, SEEK_SET);
    st_check(pi_output_import(e, o[1].buf.fd, W, H, o[1].stride, (uint64_t)real + 1, &bad) == PI_E_INVALID,
             "output claiming more bytes than the dmabuf: PI_E_INVALID", (int)(real & 0x7fffffff));
    rc = pi_output_import(e, o[1].buf.fd, W / 2, H / 2, o[1].stride / 2, o[1].buf.size, &bad);
    if (!rc) {
        f = (struct pi_frame){.bl = pics[0].bl, .el = g_fel ? pics[0].el : NULL, .rpu_frame = rpu_frame(&pics[0]),
                              .acquire_fd = -1};
        st_check(pi_render(e, &f, bad, NULL) == PI_E_INVALID, "render into an output of another size: PI_E_INVALID", 0);
        pi_output_release(e, bad);
    } else
        st_check(0, "importing a half-size output", rc);

    /* input validation */
    AVFrame *fake = av_frame_alloc();
    fake->format = AV_PIX_FMT_YUV420P10;
    fake->width = (int)W;
    fake->height = (int)H;
    f = (struct pi_frame){.bl = fake, .acquire_fd = -1};
    st_check(pi_render(e, &f, o[0].out, NULL) == PI_E_UNSUPPORTED_FORMAT, "a software frame: PI_E_UNSUPPORTED_FORMAT", 0);
    fake->format = AV_PIX_FMT_DRM_PRIME;
    fake->width = (int)W / 2;
    st_check(pi_render(e, &f, o[0].out, NULL) == PI_E_UNSUPPORTED_FORMAT, "a frame of another size: PI_E_UNSUPPORTED_FORMAT", 0);
    fake->width = (int)W;   /* DRM PRIME, no descriptor */
    st_check(pi_render(e, &f, o[0].out, NULL) == PI_E_UNSUPPORTED_FORMAT, "DRM PRIME without a descriptor: PI_E_UNSUPPORTED_FORMAT", 0);
    av_frame_free(&fake);
    AVFrame *meta = av_frame_alloc();   /* carries the RPU (pi_frame.rpu_frame) */
    f = (struct pi_frame){.bl = pics[0].bl, .el = g_fel ? pics[0].el : NULL, .rpu_frame = meta, .acquire_fd = -1};
    rc = pi_render(e, &f, o[0].out, NULL);
    st_check(rc == PI_E_BAD_RPU, "a frame without an RPU: PI_E_BAD_RPU", rc);
    AVFrameSideData *junk = av_frame_new_side_data(meta, AV_FRAME_DATA_DOVI_METADATA, 64);
    if (junk)
        memset(junk->data, 0, junk->size);
    rc = pi_render(e, &f, o[0].out, NULL);
    st_check(rc == PI_E_BAD_RPU || rc == PI_E_UNSUPPORTED_PLAN, "a junk RPU: refused, PI_E_BAD_RPU or _UNSUPPORTED_PLAN", rc);
    av_frame_free(&meta);

    /* frame lifetime: the engine holds the frames until the GPU is done */
    uint64_t want = 0, got = 0;
    for (int k = 0; k < 2; k++)   /* the first render after a stream opens refreshes the metadata */
        if (render(&pics[0], 0, &o[0]) && !pi_output_wait(e, o[0].out))
            want = out_hash(&o[0]);
    int before = av_buffer_get_ref_count(pics[0].bl->buf[0]);
    struct dv_picture c = {.bl = av_frame_clone(pics[0].bl), .el = pics[0].el ? av_frame_clone(pics[0].el) : NULL,
                           .dovi = pics[0].dovi};
    int ok = render(&c, 0, &o[0]);
    dv_picture_free(&c);   /* the caller's references gone before the GPU is done */
    int held = av_buffer_get_ref_count(pics[0].bl->buf[0]);
    if (ok && !pi_output_wait(e, o[0].out))
        got = out_hash(&o[0]);
    pi_output_done(e, o[0].out);
    int after = av_buffer_get_ref_count(pics[0].bl->buf[0]);
    st_check(ok && held == before + 1, "after pi_render the engine holds a reference to the BL frame", held - before);
    st_check(after == before, "after the render, the engine has dropped it", after - before);
    st_check(want && got == want, "the caller's frames freed right after pi_render: the same output", 0);

    /* every picture, three in flight; then the pipeline cache's growth */
    int rendered = 0;
    for (int i = 0; i < count; i++) {
        pi_output_wait(e, o[i & 1].out);
        rendered += render(&pics[i], i * period, &o[i & 1]);
    }
    pi_output_wait(e, o[0].out);
    pi_output_wait(e, o[1].out);
    st_check(rendered == count, "every picture rendered", rendered);
    if (lc.mmr) {
        /* the stream's family: 9 MMR order pairs, times the split's guard on
         * and off in the modes that use the split */
        int family = base.mmr == PI_MMR_SPLIT || base.mmr == PI_MMR_EXACT ? 18 : 9;
        for (int s = 0; s < 120 && lc.variants < family; s++) {   /* the worker (lavapipe compiles slowly) */
            struct timespec one = {1, 0};
            nanosleep(&one, NULL);
        }
        st_check(lc.variants >= family,
                 family > 16 ? "the stream's 18 P1 variants built ahead (more than the old fixed cache's 16)"
                             : "the stream's 9 P1 variants built ahead",
                 lc.variants);
    } else
        printf("selftest: (no MMR: no variants built ahead to count)\n");

    /* stream close releases the imports; a new stream works */
    pi_stream_close(e);
    st_check(lc.closed == 1 && lc.released > 0, "stream close released its imports", lc.released);
    ok = pi_stream_open(e, &si) == 0;
    for (int k = 0; k < 2 && ok; k++)   /* the second render: past the new stream's refresh */
        ok = render(&pics[0], 0, &o[0]) && !pi_output_wait(e, o[0].out);
    st_check(ok && out_hash(&o[0]) == want, "a new stream: imports again, the same output", 0);

    /* teardown with renders in flight and outputs still imported */
    for (int i = 0; i < 3 && i < count; i++)
        render(&pics[i], i * period, &o[i & 1]);
    pi_engine_destroy(e);   /* no waits, no releases first */
    g_pi = NULL;
    for (int i = 0; i < 2; i++)
        if (o[i].buf.map)
            dv_dmabuf_free(&o[i].buf);
    st_check(1, "destroy with renders in flight and outputs imported (the validation layer checks the rest)", 0);
    printf("selftest: %d of %d passed\n", st_runs - st_fails, st_runs);
    return st_fails ? 1 : 0;
}

/* --probe: what dvbench and the engine need from this machine. */
static int probe(struct pi_options *po)
{
    struct pi_probe_info info;
    int rc = pi_probe(po, &info);
    if (rc) {
        printf("probe: no device: %s\n", pi_strerror(rc));
        return 1;
    }
    printf("probe: device %s%s, output %s, release sync_file %s\n", info.device, info.cpu ? " (CPU)" : "",
           info.in_place ? "rendered in place" : "copied", info.release_fence ? "yes" : "no");
    struct pi_engine *e;
    rc = pi_engine_create(po, &e);
    if (rc) {
        printf("probe: engine: %s\n", pi_strerror(rc));
        return 1;
    }
    printf("probe: engine: %s\n", pi_engine_describe(e));
    struct pi_stream_info si = {.width = 3840, .height = 2160};
    struct out_buf o = {0};
    int ok = !pi_stream_open(e, &si) && !out_alloc(e, &o, 3840, 2160);
    printf("probe: dmabuf allocator %s; output import %s\n", dv_dmabuf_kind(), ok ? "OK" : "FAILED");
    out_free(e, &o);
    pi_engine_destroy(e);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    struct dv_source_options so = {.kind = DV_SOURCE_SW};
    int frames = 48, warmup = 8, hash = 0, probe_only = 0, frames_set = 0;
    double throughput = 0, play_seconds = 0;
    const char *file = NULL, *dump = NULL, *json = NULL;
    struct pi_options po;
    pi_options_default(&po);
    po.device = getenv("PI_DEVICE");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        int ok = 1, takes = v != NULL;
        if (!strcmp(a, "--source") && takes) {
            if (!strcmp(v, "v4l2"))
                so.kind = DV_SOURCE_V4L2;
            else if (!strcmp(v, "sw"))
                so.kind = DV_SOURCE_SW;
            else
                ok = bad_value(a, v);
            i++;
        } else if (!strcmp(a, "--no-fences"))
            g_fences = 0;
        else if (!strcmp(a, "--implicit-sync"))
            g_implicit = 1;
        else if (!strcmp(a, "--reopen"))
            g_reopen = 1;
        else if (!strcmp(a, "--kms-writeback"))
            g_kms = 1;
        else if (!strcmp(a, "--kms-play") && takes)
            ok = to_seconds(a, v, &g_kms_play), i++;
        else if (!strcmp(a, "--selftest"))
            g_selftest = 1;
        else if (!strcmp(a, "--el-sw"))
            so.el_software = 1;
        else if (!strcmp(a, "--frames") && takes) {
            ok = to_int(a, v, 1, 100000, &frames);
            frames_set = 1;
            i++;
        } else if (!strcmp(a, "--warmup") && takes)
            ok = to_int(a, v, 0, 100000, &warmup), i++;
        else if (!strcmp(a, "--start") && takes)
            ok = to_seconds(a, v, &so.start), i++;
        else if (!strcmp(a, "--fel") && takes)
            ok = to_int(a, v, 0, 1, &g_fel), i++;
        else if (!strcmp(a, "--filters") && takes) {
            if (!strcmp(v, "cb1"))
                po.filters = PI_FILTERS_CB1;
            else if (!strcmp(v, "phase") || !strcmp(v, "yblod"))
                po.filters = PI_FILTERS_PHASE;
            else
                ok = bad_value(a, v);
            i++;
        } else if (!strcmp(a, "--mmr") && takes) {
            if (!strcmp(v, "exact"))
                po.mmr = PI_MMR_EXACT;
            else if (!strcmp(v, "split"))
                po.mmr = PI_MMR_SPLIT;
            else if (!strcmp(v, "exact64"))
                po.mmr = PI_MMR_EXACT64;
            else if (!strcmp(v, "float"))
                po.mmr = PI_MMR_FLOAT;
            else
                ok = bad_value(a, v);
            i++;
        } else if (!strcmp(a, "--device") && takes)
            po.device = argv[++i];
        else if (!strcmp(a, "--pipeline-cache") && takes)
            po.pipeline_cache = argv[++i];
        else if (!strcmp(a, "--throughput") && takes)
            ok = to_seconds(a, v, &throughput), i++;
        else if (!strcmp(a, "--play") && takes)
            ok = to_seconds(a, v, &play_seconds), i++;
        else if (!strcmp(a, "--hash"))
            hash = 1;
        else if (!strcmp(a, "--dump") && takes)
            dump = argv[++i];
        else if (!strcmp(a, "--json") && takes)
            json = argv[++i];
        else if (!strcmp(a, "--measure") && takes)
            ok = measure_list(a, v, &po.measure), i++;
        else if (!strcmp(a, "--probe"))
            probe_only = 1;
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (a[0] != '-' && !file)
            file = a;
        else {
            fprintf(stderr, "dvbench: unknown option or missing value: %s\n", a);
            usage();
            return 2;
        }
        if (!ok)
            return 2;
    }
    if (env_on("PI_DEBUG"))
        po.log_level = PI_LOG_DEBUG;
    po.profile = env_on("PI_PROFILE");
    for (int k = 0; k < NKNOBS; k++)
        if (env_on(knobs[k].env))
            po.measure |= knobs[k].flag;
    if (getenv("PI_ABLATE"))
        po.ablate = (unsigned)atoi(getenv("PI_ABLATE"));
    if (probe_only)
        return probe(&po);
    if (!file) {
        usage();
        return 2;
    }
    /* The Pi's HEVC decoder has at most 32 capture buffers and the bench
     * holds every preloaded picture: keep it under. */
    if (so.kind == DV_SOURCE_V4L2 && frames > 12) {
        if (frames_set)
            fprintf(stderr, "dvbench: --frames %d -> 12 (the hardware decoder's buffer pool)\n", frames);
        frames = 12;
    }
    if (warmup >= frames)
        warmup = frames > 4 ? 2 : 0;
    struct rlimit nofile;
    if (!getrlimit(RLIMIT_NOFILE, &nofile) && nofile.rlim_cur < nofile.rlim_max) {
        nofile.rlim_cur = nofile.rlim_max;
        setrlimit(RLIMIT_NOFILE, &nofile);
    }
    so.extra_frames = so.kind == DV_SOURCE_V4L2 ? frames + 2 : frames + 8;

    /* 1. Decode: every picture first, timed, held for the engine; the first
     * picture's size is the stream's. */
    struct dv_source *src = dv_source_open(file, &so);
    if (!src)
        return 1;
    g_profile = dv_source_profile(src);
    struct dv_picture *pics = calloc((size_t)frames, sizeof(*pics));
    if (!pics)
        return 1;
    int count = 0;
    double d0 = now_ms(), dc0 = cpu_ms();
    while (count < frames) {
        int r = dv_source_next(src, &pics[count]);
        if (r <= 0)
            break;
        if (!pics[count].dovi ||
            (count && (pics[count].bl->width != pics[0].bl->width || pics[count].bl->height != pics[0].bl->height))) {
            dv_picture_free(&pics[count]);
            continue;
        }
        count++;
    }
    double decode_ms = now_ms() - d0, decode_cpu = cpu_ms() - dc0, copy_ms = dv_source_copy_ms(src);
    if (!count) {
        fprintf(stderr, "dvbench: no Dolby Vision pictures decoded\n");
        return 1;
    }
    const AVFrame *b0 = pics[0].bl;
    uint32_t W = (uint32_t)b0->width, H = (uint32_t)b0->height;
    printf("source: %s, profile %d, %ux%u%s, %s SAND128 P030\n", file, g_profile, W, H, pics[0].el ? " + EL" : "",
           so.kind == DV_SOURCE_V4L2 ? "v4l2 request" : "software decode");
    printf("decode: %d pictures in %.0f ms, %.1f fps (CPU %.1f ms/picture%s)\n", count, decode_ms,
           count * 1e3 / decode_ms, decode_cpu / count,
           so.kind == DV_SOURCE_SW ? ", includes the copy into dmabufs" : "");
    if (so.kind == DV_SOURCE_SW)
        printf("decode: of that, %.1f ms/picture copying into SAND128 dmabufs\n", copy_ms / count);
    AVRational rate = dv_source_rate(src);
    double period = rate.num > 0 ? (double)rate.den / rate.num : 1.0 / 24;

    if (g_selftest) {
        int r = selftest(po, pics, count, W, H, period);
        for (int i = 0; i < count; i++)
            dv_picture_free(&pics[i]);
        dv_source_close(src);
        free(pics);
        return r;
    }

    /* 2. The engine, the stream and three outputs. */
    int rc = pi_engine_create(&po, &g_pi);
    if (rc) {
        fprintf(stderr, "pi: engine didn't start: %s\n", pi_strerror(rc));
        return 1;
    }
    struct pi_stream_info si = {.width = W, .height = H, .profile = g_profile};
    if ((rc = pi_stream_open(g_pi, &si))) {
        fprintf(stderr, "pi: stream %ux%u: %s\n", W, H, pi_strerror(rc));
        return 1;
    }
    struct out_buf out[3];
    memset(out, 0, sizeof(out));
    for (unsigned i = 0; i < 3; i++)
        if (out_alloc(g_pi, &out[i], W, H)) {
            fprintf(stderr, "pi: output %u failed\n", i);
            return 1;
        }
    const char *filters = po.filters == PI_FILTERS_CB1 ? "cb1" : "phase";
    printf("engine: %s, filters %s\n", pi_engine_describe(g_pi), filters);

    /* 3. Per picture, synchronous. */
    int status = 0, failed = 0, timed = 0;
    double *times = calloc((size_t)count, sizeof(double)), pass_sum[2] = {0}, plan_sum = 0;
    if (!times)
        return 1;
    int implicit_busy = 0, implicit_idle = 0, implicit_later = 0, gpu_later = 0;
    uint64_t *first_hash = calloc((size_t)count, sizeof(uint64_t));
    for (int i = 0; i < count; i++) {
        double t0 = now_ms();
        int ok = render(&pics[i], i * period, &out[0]);
        if (ok && g_implicit) {   /* POLLOUT: every fence on the dmabuf (read and write) signalled */
            struct pollfd pf = {.fd = out[0].buf.fd, .events = POLLOUT};
            implicit_busy += poll(&pf, 1, 0) == 0;
            struct timespec ms2 = {0, 2000000};
            nanosleep(&ms2, NULL);
            implicit_later += !pi_output_done(g_pi, out[0].out) && poll(&pf, 1, 0) == 0;
            gpu_later += !pi_output_done(g_pi, out[0].out);
            pi_output_wait(g_pi, out[0].out);
            implicit_idle += poll(&pf, 1, 0) > 0;
        }
        ok = ok && pi_output_wait(g_pi, out[0].out) == 0;
        double t = now_ms() - t0;
        if (!ok) {
            failed++;
            continue;
        }
        if (i >= warmup) {
            times[timed++] = t;
            struct pi_render_stats st;
            pi_output_stats(g_pi, out[0].out, &st);
            for (int k = 0; k < 2; k++)
                pass_sum[k] += st.pass_ms[k];
            plan_sum += st.plan_ms;
        }
        if (hash || g_reopen)
            first_hash[i] = out_hash(&out[0]);
        if (hash)
            printf("hash %03d %016llx\n", i, (unsigned long long)first_hash[i]);
        if (dump && out_dump(&out[0], dump, i))
            status = 1;
    }
    struct stats s = summarize(times, timed);
    printf("per picture: median %.2f ms, p95 %.2f, mean %.2f (%d pictures%s), %.1f fps%s\n", s.median, s.p95, s.mean,
           s.n, warmup ? " after warmup" : "", s.median > 0 ? 1e3 / s.median : 0, failed ? " FAILURES" : "");
    if (s.n) {
        printf("of which planning (CPU): %.2f ms", plan_sum / s.n);
        if (pass_sum[0] + pass_sum[1] > 0)
            printf("; GPU passes (PI_PROFILE): P1 chroma %.2f, P2 4:2:2 + pack %.2f ms", pass_sum[0] / s.n,
                   pass_sum[1] / s.n);
        printf("\n");
    }
    if (g_kms) {
        struct kms k;
        if (kms_open(&k, &out[0])) {
            printf("kms: no writeback setup (see stderr)\n");
            status = 1;
        } else {
            int marked_f, marked_n;
            int fenced = kms_run(&k, pics, count, period, &out[0], 1, &marked_f);
            int bare = kms_run(&k, pics, count, period, &out[0], 0, &marked_n);
            printf("kms: IN_FENCE_FD: %d of %d captures are the finished frame (%d show the unfinished marker); "
                   "without a fence: %d (%d)\n", fenced, count - failed, marked_f, bare, marked_n);
            if (fenced != count - failed)
                status = 1;
        }
        kms_close(&k);
    }
    if (g_reopen) {
        pi_stream_close(g_pi);
        int same = 0, done = 0;
        if (pi_stream_open(g_pi, &si) == 0)
            for (int i = 0; i < count; i++)
                if (render(&pics[i], i * period, &out[0]) && pi_output_wait(g_pi, out[0].out) == 0) {
                    done++;
                    same += first_hash[i] && out_hash(&out[0]) == first_hash[i];
                }
        printf("reopen: %d of %d pictures identical to the first stream's%s\n", same, count - failed,
               same == count - failed && done == same ? "" : " FAILURES");
        if (same != count - failed)
            status = 1;
    }
    if (g_implicit)
        printf("implicit sync: the output dmabuf busy right after submit for %d of %d pictures; 2 ms later for %d "
               "of the %d still rendering; idle after completion for %d\n", implicit_busy, count - failed,
               implicit_later, gpu_later, implicit_idle);
    if (failed) {
        printf("%d pictures failed\n", failed);
        status = 1;
    }
    FILE *js = NULL;
    if (json && !(js = fopen(json, "w"))) {
        fprintf(stderr, "dvbench: %s: %s\n", json, strerror(errno));
        status = 1;
    }
    if (js) {
        fprintf(js, "{\"file\": ");
        json_string(js, file);
        fprintf(js, ", \"profile\": %d, \"width\": %u, \"height\": %u, \"el\": %s, \"source\": \"%s\", \"engine\": ",
                g_profile, W, H, pics[0].el ? "true" : "false", so.kind == DV_SOURCE_V4L2 ? "v4l2" : "sw");
        json_string(js, pi_engine_describe(g_pi));
        fprintf(js, ", \"filters\": \"%s\", \"pictures\": %d, \"decode_fps\": %.2f, \"decode_cpu_ms\": %.2f, "
                    "\"median_ms\": %.3f, \"p95_ms\": %.3f, \"mean_ms\": %.3f, \"timed\": %d, \"failed\": %d, "
                    "\"plan_ms\": %.3f",
                filters, count, count * 1e3 / decode_ms, decode_cpu / count, s.median, s.p95, s.mean, s.n, failed,
                s.n ? plan_sum / s.n : 0);
        if (pass_sum[0] + pass_sum[1] > 0)
            fprintf(js, ", \"p1_ms\": %.3f, \"p2_ms\": %.3f", pass_sum[0] / s.n, pass_sum[1] / s.n);
    }

    /* 4. Throughput: flat out, three frames in flight as in playback. */
    if (throughput > 0) {
        long done = 0, tfail = 0;
        double t0 = now_ms(), c0 = cpu_ms(), end = t0 + throughput * 1e3;
        for (long i = 0; now_ms() < end; i++) {
            struct out_buf *o = &out[i % 3];
            pi_output_wait(g_pi, o->out);
            int ok = render(&pics[i % count], i * period, o);
            done += ok;
            tfail += !ok;
        }
        for (unsigned k = 0; k < 3; k++)
            pi_output_wait(g_pi, out[k].out);
        double wall = now_ms() - t0, cpu = cpu_ms() - c0;
        printf("throughput: %.1f fps over %.1f s, CPU %.2f ms/picture%s\n", done * 1e3 / wall, wall / 1e3,
               done ? cpu / done : 0, tfail ? " FAILURES" : "");
        if (js)
            fprintf(js, ", \"throughput_fps\": %.2f, \"throughput_cpu_ms\": %.3f", done * 1e3 / wall,
                    done ? cpu / done : 0);
        if (tfail)
            status = 1;
    }

    /* 5. Real-time playback from a decoding thread. */
    if (g_kms_play > 0 && kms_play(file, so, g_kms_play, out))
        status = 1;
    if (play_seconds > 0) {
        struct play_result r;
        if (play(file, so, play_seconds, out, &r)) {
            printf("play: source failed\n");
            status = 1;
        } else {
            printf("play: %ld pictures in %.1f s at the stream rate, %ld late (worst %.1f ms), %ld decode stalls, "
                   "CPU %.2f ms/picture%s%s\n",
                   r.shown, r.wall_ms / 1e3, r.late, r.worst_late_ms, r.starved, r.shown ? r.cpu_ms / r.shown : 0,
                   r.fenced ? ", sync_file fences" : "", r.failed ? " FAILURES" : "");
            if (js)
                fprintf(js, ", \"play_shown\": %ld, \"play_late\": %ld, \"play_worst_late_ms\": %.2f, "
                            "\"play_decode_stalls\": %ld, \"play_cpu_ms\": %.3f",
                        r.shown, r.late, r.worst_late_ms, r.starved, r.shown ? r.cpu_ms / r.shown : 0);
            if (r.failed)
                status = 1;
        }
    }
    if (js) {
        fprintf(js, "}\n");
        if (fclose(js)) {
            fprintf(stderr, "dvbench: %s: write failed\n", json);
            status = 1;
        }
    }
    for (int i = 0; i < count; i++)
        dv_picture_free(&pics[i]);
    for (unsigned i = 0; i < 3; i++)
        out_free(g_pi, &out[i]);
    pi_stream_close(g_pi);
    pi_engine_destroy(g_pi);
    dv_source_close(src);
    free(times);
    free(first_hash);
    free(pics);
    return status;
}
