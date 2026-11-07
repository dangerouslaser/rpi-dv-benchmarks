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
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "source.h"
#include "alloc.h"
#include "pi_engine.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
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
static int g_profile, g_fel = 1, g_fences = 1;

/* acquire_fd: taken; release_fd: NULL, or the render's sync_file (-1 none). */
static int render_fenced(const struct dv_picture *q, double pts, struct out_buf *o, int acquire_fd, int *release_fd)
{
    struct pi_frame f = {.bl = q->bl, .el = g_fel ? q->el : NULL, .rpu = q->dovi->data, .rpu_size = q->dovi->size,
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

/* --probe: what dvbench and the engine need from this machine. */
static int probe(struct pi_options *po)
{
    struct pi_engine *e;
    int rc = pi_engine_create(po, &e);
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
    for (int i = 0; i < count; i++) {
        double t0 = now_ms();
        int ok = render(&pics[i], i * period, &out[0]) && pi_output_wait(g_pi, out[0].out) == 0;
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
        if (hash)
            printf("hash %03d %016llx\n", i, (unsigned long long)out_hash(&out[0]));
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
    free(pics);
    return status;
}
