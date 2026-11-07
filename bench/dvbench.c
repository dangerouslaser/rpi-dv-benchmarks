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
 *                        4:2:0 reshaping and Annex B 4:2:2 filters) or yblod
 *                        (YBLOD's chroma phase filters, 2-tap 4:2:2, the
 *                        full-height single-layer MMR layout)
 *     --mmr M            chroma MMR: exact (default: float with an error bound,
 *                        exact where it could matter), split, exact64,
 *                        exact32 (all exact), float (within 2 codes)
 *     --device NAME      Vulkan device name substring (default: the first GPU)
 *     --throughput S     then flat out over the decoded pictures for S
 *                        seconds, three frames in flight
 *     --play S           then real-time playback for S seconds: a decoding
 *                        thread (8-picture queue) and rendering on the
 *                        stream's frame clock; counts pictures that missed
 *                        their display deadline and decode stalls
 *     --hash             print a hash of every output picture
 *     --dump DIR         write each output as DIR/pi-N.bgra
 *     --json FILE        machine-readable summary
 *   Environment: PI_PROFILE=1 per-pass GPU times; PI_DEBUG=1 the plan.
 *
 * Per picture the engine plans, imports (if new), renders and finishes
 * before the next starts: wall time from submit to completion. Outputs are
 * the HDMI tunnel's 3840x2160 BGRA (12-bit 4:2:2 packed with the metadata).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "source.h"
#include "alloc.h"
#include "pi_engine.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define W 3840
#define H 2160

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

/* ---- output buffers: linear BGRA 3840x2160 dmabufs ----------------------- */

struct out_buf {
    struct dv_dmabuf buf;
    int fd;
    uint32_t stride;
};

static int out_alloc(struct out_buf *o)
{
    o->stride = W * 4;
    /* A row more than the picture (the engine sees the buffer as two images
     * of every other row, the odd rows' a row in) and a page: drivers pad
     * buffer requirements, which must fit in the dmabuf. */
    if (dv_dmabuf_alloc(&o->buf, (size_t)o->stride * (H + 1) + 4096))
        return -1;
    o->fd = o->buf.fd;
    return 0;
}

/* Over the picture only (the allocation is larger). */
static uint64_t out_hash(const struct out_buf *o)
{
    dv_dmabuf_cpu(&o->buf, 1, 0);
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t *p = (const uint64_t *)o->buf.map;
    for (size_t i = 0; i < (size_t)o->stride * H / 8; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    dv_dmabuf_cpu(&o->buf, 0, 0);
    return h;
}

static void out_dump(const struct out_buf *o, const char *dir, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/pi-%03d.bgra", dir, n);
    FILE *f = fopen(path, "wb");
    if (f) {
        dv_dmabuf_cpu(&o->buf, 1, 0);
        fwrite(o->buf.map, 1, (size_t)o->stride * H, f);
        dv_dmabuf_cpu(&o->buf, 0, 0);
        fclose(f);
    }
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

/* ---- one picture ------------------------------------------------------------ */

static struct pi_engine *g_pi;
static int g_profile, g_fel = 1;

/* The stream's chroma siting, as Kodi's DV adapters read it: Profile 7
 * without one is top-left. */
static int top_left(const AVFrame *f)
{
    return f->chroma_location == AVCHROMA_LOC_TOPLEFT ||
           (f->chroma_location == AVCHROMA_LOC_UNSPECIFIED && g_profile == 7);
}

static int submit(const struct dv_picture *q, double pts, long i, unsigned slot)
{
    int rc = pi_submit(g_pi, q->bl, g_fel ? q->el : NULL, q->dovi->data, q->dovi->size, top_left(q->bl),
                       (int64_t)(pts * 1e6), i == 0, slot);
    static int reported;
    if (rc && !reported++)
        fprintf(stderr, "pi: submit returned %d (>0: the planner's status, or a frame outside this engine)\n", rc);
    return rc == 0;
}

/* ---- real-time playback ------------------------------------------------------ */

/* --play: a decode thread fills a player-sized queue while the render loop
 * runs on the stream's frame clock: picture k is due on screen at tick k + 1
 * (one frame of latency, as a display flip). Counts pictures whose render
 * missed that deadline and ticks at which decoding hadn't delivered yet. */
#define PLAY_QUEUE 8

struct play_queue {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct dv_picture q[PLAY_QUEUE];
    int head, count, eof, stop;
    struct dv_source *src;
};

static void *play_decoder(void *arg)
{
    struct play_queue *pq = arg;
    for (;;) {
        struct dv_picture p;
        int r = dv_source_next(pq->src, &p);
        if (r > 0 && (!p.dovi || p.bl->width > W || p.bl->height > H)) {
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
    double worst_late_ms, cpu_ms, wall_ms;
};

static int play(const char *file, struct dv_source_options so, double seconds, struct play_result *res)
{
    memset(res, 0, sizeof(*res));
    so.extra_frames = PLAY_QUEUE + 6;
    struct play_queue pq = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
    pq.src = dv_source_open(file, &so);
    if (!pq.src)
        return -1;
    AVRational rate = dv_source_rate(pq.src);
    double period = rate.num > 0 ? 1e3 * rate.den / rate.num : 1e3 / 24;
    pthread_t thread;
    pthread_create(&thread, NULL, play_decoder, &pq);
    /* Start once half the queue is buffered, as a player prerolls. */
    pthread_mutex_lock(&pq.lock);
    while (pq.count < PLAY_QUEUE / 2 && !pq.eof)
        pthread_cond_wait(&pq.cond, &pq.lock);
    pthread_mutex_unlock(&pq.lock);

    /* In flight: the picture each output slot shows, and its deadline. */
    struct dv_picture held[3] = {{0}};
    int pending[3] = {0};
    double due[3] = {0};
    double t0 = now_ms(), c0 = cpu_ms();
    long frames = (long)(seconds * 1e3 / period);
    for (long k = 0; k < frames; k++) {
        double tick = t0 + k * period;
        sleep_until_ms(tick);
        unsigned slot = (unsigned)(k % 3);
        /* The picture submitted three ticks ago must be done before its
         * slot is reused; whether it met its own deadline was judged then. */
        if (pending[slot]) {
            pi_wait(g_pi, slot);
            dv_picture_free(&held[slot]);
            pending[slot] = 0;
        }
        /* Deadline check of the previous picture: due at this tick. */
        unsigned prev = (unsigned)((k + 2) % 3);
        if (k > 0 && pending[prev] && !pi_done(g_pi, prev)) {
            res->late++;
            pi_wait(g_pi, prev);
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
            break;
        }
        struct dv_picture p = pq.q[pq.head];
        pq.head = (pq.head + 1) % PLAY_QUEUE;
        pq.count--;
        pthread_cond_broadcast(&pq.cond);
        pthread_mutex_unlock(&pq.lock);

        if (!submit(&p, k * period / 1e3, k, slot)) {
            res->failed++;
            dv_picture_free(&p);
            continue;
        }
        held[slot] = p;
        pending[slot] = 1;
        due[slot] = tick + period;
        res->shown++;
    }
    for (unsigned s = 0; s < 3; s++)
        if (pending[s]) {
            pi_wait(g_pi, s);
            dv_picture_free(&held[s]);
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

/* ---- main --------------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr, "usage: dvbench [--source sw|v4l2] [--el-sw] [--frames N] [--warmup N] [--start S] [--fel 0|1]\n"
                    "               [--filters cb1|yblod] [--mmr exact|split|exact64|exact32|float] [--device NAME]\n"
                    "               [--throughput S] [--play S] [--hash] [--dump DIR] [--json FILE] FILE\n");
}

int main(int argc, char **argv)
{
    struct dv_source_options so = {.kind = DV_SOURCE_SW, .layout = DV_LAYOUT_SAND30};
    int frames = 48, warmup = 8, contract = 1, hash = 0;
    enum pi_mmr_mode mode = PI_MMR_BOUNDED;
    double throughput = 0, play_seconds = 0;
    const char *file = NULL, *dump = NULL, *json = NULL, *device = getenv("PI_DEVICE");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--source") && v)
            so.kind = !strcmp(argv[++i], "v4l2") ? DV_SOURCE_V4L2 : DV_SOURCE_SW;
        else if (!strcmp(a, "--el-sw"))
            so.el_software = 1;
        else if (!strcmp(a, "--frames") && v)
            frames = atoi(argv[++i]);
        else if (!strcmp(a, "--warmup") && v)
            warmup = atoi(argv[++i]);
        else if (!strcmp(a, "--start") && v)
            so.start = atof(argv[++i]);
        else if (!strcmp(a, "--fel") && v)
            g_fel = atoi(argv[++i]);
        else if (!strcmp(a, "--filters") && v)
            contract = strcmp(argv[++i], "yblod") != 0;
        else if (!strcmp(a, "--mmr") && v) {
            const char *m = argv[++i];
            mode = !strcmp(m, "float")     ? PI_MMR_FLOAT
                 : !strcmp(m, "split")     ? PI_MMR_HYBRID
                 : !strcmp(m, "exact64")   ? PI_MMR_EXACT64
                 : !strcmp(m, "exact32")   ? PI_MMR_EXACT32
                                           : PI_MMR_BOUNDED;
        } else if (!strcmp(a, "--device") && v)
            device = argv[++i];
        else if (!strcmp(a, "--throughput") && v)
            throughput = atof(argv[++i]);
        else if (!strcmp(a, "--play") && v)
            play_seconds = atof(argv[++i]);
        else if (!strcmp(a, "--hash"))
            hash = 1;
        else if (!strcmp(a, "--dump") && v)
            dump = argv[++i];
        else if (!strcmp(a, "--json") && v)
            json = argv[++i];
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (a[0] != '-')
            file = a;
        else {
            fprintf(stderr, "dvbench: unknown option %s\n", a);
            usage();
            return 2;
        }
    }
    if (!file) {
        usage();
        return 2;
    }
    /* The Pi's HEVC decoder has at most 32 capture buffers and the bench
     * holds every preloaded picture: keep it under. */
    if (so.kind == DV_SOURCE_V4L2 && frames > 12) {
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

    /* 1. Decode: every picture first, timed, held for the engine. */
    struct dv_source *src = dv_source_open(file, &so);
    if (!src)
        return 1;
    g_profile = dv_source_profile(src);
    struct dv_picture *pics = calloc((size_t)frames, sizeof(*pics));
    int count = 0;
    double d0 = now_ms(), dc0 = cpu_ms();
    while (count < frames) {
        int r = dv_source_next(src, &pics[count]);
        if (r <= 0)
            break;
        if (!pics[count].dovi || pics[count].bl->width > W || pics[count].bl->height > H) {
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
    printf("source: %s, profile %d, %dx%d%s, %s SAND128 P030\n", file, g_profile, b0->width, b0->height,
           pics[0].el ? " + EL" : "", so.kind == DV_SOURCE_V4L2 ? "v4l2 request" : "software decode");
    printf("decode: %d pictures in %.0f ms, %.1f fps (CPU %.1f ms/picture%s)\n", count, decode_ms,
           count * 1e3 / decode_ms, decode_cpu / count,
           so.kind == DV_SOURCE_SW ? ", includes the copy into dmabufs" : "");
    if (so.kind == DV_SOURCE_SW)
        printf("decode: of that, %.1f ms/picture copying into SAND128 dmabufs\n", copy_ms / count);
    AVRational rate = dv_source_rate(src);
    double period = rate.num > 0 ? (double)rate.den / rate.num : 1.0 / 24;

    /* 2. The engine and its three output buffers. */
    struct out_buf out[3];
    memset(out, 0, sizeof(out));
    g_pi = pi_create(device, mode, contract);
    if (!g_pi) {
        fprintf(stderr, "pi: engine didn't start\n");
        return 1;
    }
    for (unsigned i = 0; i < 3; i++)
        if (out_alloc(&out[i]) || pi_set_output(g_pi, i, out[i].fd, out[i].stride)) {
            fprintf(stderr, "pi: output %u failed\n", i);
            return 1;
        }
    printf("engine: %s, filters %s\n", pi_describe(g_pi), contract ? "cb1" : "yblod");

    /* 3. Per picture, synchronous. */
    int status = 0, failed = 0, timed = 0;
    double *times = calloc((size_t)count, sizeof(double)), pass_sum[3] = {0}, plan_sum = 0;
    for (int i = 0; i < count; i++) {
        double t0 = now_ms();
        int ok = submit(&pics[i], i * period, i, 0) && pi_wait(g_pi, 0) == 0;
        double t = now_ms() - t0;
        if (!ok) {
            failed++;
            continue;
        }
        if (i >= warmup) {
            times[timed++] = t;
            double pm[3], plm;
            pi_times(g_pi, 0, pm, &plm);
            for (int k = 0; k < 3; k++)
                pass_sum[k] += pm[k];
            plan_sum += plm;
        }
        if (hash)
            printf("hash %03d %016llx\n", i, (unsigned long long)out_hash(&out[0]));
        if (dump)
            out_dump(&out[0], dump, i);
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
    FILE *js = json ? fopen(json, "w") : NULL;
    if (js)
        fprintf(js, "{\"file\": \"%s\", \"profile\": %d, \"width\": %d, \"height\": %d, \"el\": %s, \"source\": \"%s\", "
                    "\"engine\": \"%s\", \"filters\": \"%s\", \"pictures\": %d, \"decode_fps\": %.2f, "
                    "\"decode_cpu_ms\": %.2f, \"median_ms\": %.3f, \"p95_ms\": %.3f, \"mean_ms\": %.3f, \"timed\": %d, "
                    "\"failed\": %d, \"plan_ms\": %.3f",
                file, g_profile, b0->width, b0->height, pics[0].el ? "true" : "false",
                so.kind == DV_SOURCE_V4L2 ? "v4l2" : "sw", pi_describe(g_pi), contract ? "cb1" : "yblod", count,
                count * 1e3 / decode_ms, decode_cpu / count, s.median, s.p95, s.mean, s.n, failed,
                s.n ? plan_sum / s.n : 0);
    if (js && pass_sum[0] + pass_sum[1] > 0)
        fprintf(js, ", \"p1_ms\": %.3f, \"p2_ms\": %.3f", pass_sum[0] / s.n, pass_sum[1] / s.n);

    /* 4. Throughput: flat out, three frames in flight as in playback. */
    if (throughput > 0) {
        long done = 0, tfail = 0;
        double t0 = now_ms(), c0 = cpu_ms(), end = t0 + throughput * 1e3;
        for (long i = 0; now_ms() < end; i++) {
            int ok = submit(&pics[i % count], i * period, i, (unsigned)(i % 3));
            done += ok;
            tfail += !ok;
        }
        for (unsigned k = 0; k < 3; k++)
            pi_wait(g_pi, k);
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
        if (play(file, so, play_seconds, &r)) {
            printf("play: source failed\n");
            status = 1;
        } else {
            printf("play: %ld pictures in %.1f s at the stream rate, %ld late (worst %.1f ms), %ld decode stalls, "
                   "CPU %.2f ms/picture%s\n",
                   r.shown, r.wall_ms / 1e3, r.late, r.worst_late_ms, r.starved, r.shown ? r.cpu_ms / r.shown : 0,
                   r.failed ? " FAILURES" : "");
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
        fclose(js);
    }
    for (int i = 0; i < count; i++)
        dv_picture_free(&pics[i]);
    pi_destroy(g_pi);
    dv_source_close(src);
    return status;
}
