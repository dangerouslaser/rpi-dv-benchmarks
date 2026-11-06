/* dvbench: Dolby Vision decode and frame generation on the Raspberry Pi 5,
 * yblod's Vulkan engine against CroqueMr's CB1 (libplacebo on GLES), on the
 * same decoded pictures. Headless: no display, no audio. A port of
 * dvvk-mr-compare (intel-dv-libreelec) from VAAPI to DRM PRIME frames.
 *
 *   dvbench [options] FILE
 *     --source sw|v4l2   decoder: FFmpeg software into udmabufs (any Linux;
 *                        default) or the Pi's V4L2 request HEVC (SAND128)
 *     --layout p010|sand software pictures' layout (default sand: the Pi's)
 *     --el-sw            v4l2: decode the Profile 7 EL in software
 *     --engines LIST     cb1,vk (default both)
 *     --frames N         pictures decoded and rendered (default 48)
 *     --warmup N         pictures left out of the per-picture timing (default 8)
 *     --start S          seek to S seconds first
 *     --fel 0|1          give both engines the EL (default 1)
 *     --yblod            Vulkan engine as YBLOD runs it, not CB1's contract
 *     --el-scaler S      Vulkan engine's FEL EL upscale: annexb (default, as
 *                        Master Reference) or bilinear (cheaper)
 *     --cb1-mode M       CB1: master (Master Reference, default) or reference
 *                        (its performance mode; output not comparable byte
 *                        for byte with the Vulkan engine's)
 *     --cb1-fel F        CB1 reference mode's FEL EL filter: default,
 *                        bilinear, reference
 *     --throughput S     then each engine flat out over the decoded pictures
 *                        for S seconds, pipelined as in playback
 *     --play S           then real-time playback for S seconds: a decoding
 *                        thread (8-picture queue) and rendering on the
 *                        stream's frame clock; counts pictures that missed
 *                        their display deadline and decode stalls
 *     --node PATH        Vulkan's DRM render node (default: none, any device;
 *                        on the Pi the V3D node)
 *     --hash             print a hash of every output picture (A/B checks)
 *     --dump DIR         write each output as DIR/ENGINE-N.bgra
 *     --json FILE        machine-readable summary
 *
 * Per picture each engine imports (if new), renders and finishes before the
 * next starts: wall time from submit to completion, CPU-clocked. Outputs are
 * the HDMI tunnel's 3840x2160 BGRA (12-bit 4:2:2 packed with metadata).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "source.h"
#include "alloc.h"
#include "dv_engine_vk.h"
#include "dvbridge_render.h"
#include "dvbridge_dmabuf.h"
#include "dvbridge_gl_frame.h"
#include "dvbridge_placebo.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libplacebo/opengl.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

/* ---- the Vulkan engine, dlopened as Kodi does -------------------------- */

static struct {
    uint32_t (*abi)(void);
    int (*create)(int, void *, dvvk_engine **);
    int (*set_output)(dvvk_engine *, uint32_t, int, uint32_t);
    int (*set_surface)(dvvk_engine *, uint32_t, const dvvk_surface_dmabuf *);
    int (*submit)(dvvk_engine *, const dvvk_frame *, uint64_t *);
    int (*wait)(dvvk_engine *, uint64_t, uint64_t);
    void (*destroy)(dvvk_engine *);
} vk;

static int load_vk(void)
{
    void *h = dlopen(DVVK_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "vk: %s\n", dlerror());
        return -1;
    }
    vk.abi = dlsym(h, "dvvk_abi_version");
    vk.create = dlsym(h, "dvvk_create");
    vk.set_output = dlsym(h, "dvvk_set_output");
    vk.set_surface = dlsym(h, "dvvk_set_surface_dmabuf");
    vk.submit = dlsym(h, "dvvk_submit");
    vk.wait = dlsym(h, "dvvk_wait");
    vk.destroy = dlsym(h, "dvvk_destroy");
    if (!vk.abi || vk.abi() != DVVK_ABI_VERSION || !vk.create || !vk.set_output || !vk.set_surface ||
        !vk.submit || !vk.wait) {
        fprintf(stderr, "vk: %s lacks the DRM PRIME input (dvvk_set_surface_dmabuf) or ABI %u\n", DVVK_LIBRARY,
                DVVK_ABI_VERSION);
        return -1;
    }
    return 0;
}

/* ---- output buffers: linear BGRA 3840x2160 udmabufs ---------------------- */

struct out_buf {
    struct dv_dmabuf buf;
    int fd;
    uint32_t stride;
};

static int out_alloc(struct out_buf *o)
{
    o->stride = W * 4;
    /* A page more than the picture: drivers pad buffer requirements (v3dv:
     * a 64-byte read-ahead), which must fit in the dmabuf. */
    if (dv_dmabuf_alloc(&o->buf, (size_t)o->stride * H + 4096))
        return -1;
    o->fd = o->buf.fd;
    return 0;
}

/* Over the picture only (the allocation may be rounded up). */
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

static void out_dump(const struct out_buf *o, const char *dir, const char *engine, int n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%03d.bgra", dir, engine, n);
    FILE *f = fopen(path, "wb");
    if (f) {
        dv_dmabuf_cpu(&o->buf, 1, 0);
        fwrite(o->buf.map, 1, (size_t)o->stride * H, f);
        dv_dmabuf_cpu(&o->buf, 0, 0);
        fclose(f);
    }
}

/* ---- dmabuf identity: one id per decoded buffer ------------------------ */

static uint64_t inode_of(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? 0 : (uint64_t)st.st_ino;
}

static uint64_t ids[1024];
static int id_count;

static uint32_t surface_id(const AVFrame *f)
{
    uint64_t ino = inode_of(dv_drm(f)->objects[0].fd);
    for (int i = 0; i < id_count; i++)
        if (ids[i] == ino)
            return (uint32_t)i + 1;
    ids[id_count] = ino;
    return (uint32_t)++id_count;
}

/* The engine's registration of a DRM PRIME picture (one layer, two planes). */
static int vk_register(dvvk_engine *e, const AVFrame *f, uint32_t id)
{
    const AVDRMFrameDescriptor *d = dv_drm(f);
    const AVDRMLayerDescriptor *l = &d->layers[0];
    if (d->nb_layers != 1 || l->nb_planes != 2 || d->nb_objects > 2)
        return -1;
    dvvk_surface_dmabuf s = {.drm_format = l->format, .width = (uint32_t)f->width, .height = (uint32_t)f->height,
                             .num_objects = (uint32_t)d->nb_objects, .modifier = d->objects[0].format_modifier};
    for (int i = 0; i < d->nb_objects; i++) {
        s.fd[i] = d->objects[i].fd;
        s.size[i] = (uint32_t)d->objects[i].size;
    }
    for (int p = 0; p < 2; p++) {
        s.object[p] = (uint32_t)l->planes[p].object_index;
        s.offset[p] = (uint32_t)l->planes[p].offset;
        s.pitch[p] = (uint32_t)l->planes[p].pitch;
    }
    return vk.set_surface(e, id, &s);
}

/* ---- CB1's inputs -------------------------------------------------------- */

/* Kodi's CB1 adapter (DVBridgeGLES.cpp SourceChroma). */
static enum pl_chroma_location cb1_chroma(enum AVChromaLocation loc, int profile)
{
    if (loc == AVCHROMA_LOC_UNSPECIFIED && profile == 7)
        return PL_CHROMA_TOP_LEFT;
    if (loc == AVCHROMA_LOC_TOPLEFT)
        return PL_CHROMA_TOP_LEFT;
    if (loc == AVCHROMA_LOC_CENTER)
        return PL_CHROMA_CENTER;
    if (loc == AVCHROMA_LOC_TOP)
        return PL_CHROMA_TOP_CENTER;
    if (loc == AVCHROMA_LOC_BOTTOMLEFT)
        return PL_CHROMA_BOTTOM_LEFT;
    if (loc == AVCHROMA_LOC_BOTTOM)
        return PL_CHROMA_BOTTOM_CENTER;
    return PL_CHROMA_LEFT;
}

static PFNEGLCREATEIMAGEKHRPROC egl_create_image;
static PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target;
static EGLDisplay egl_dpy;

static GLuint dmabuf_texture(int fd, uint32_t fourcc, int w, int h, uint32_t offset, uint32_t pitch, EGLImageKHR *img)
{
    const EGLint a[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
                        EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
                        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch, EGL_NONE};
    *img = egl_create_image(egl_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
    if (!*img)
        return 0;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    gl_image_target(GL_TEXTURE_2D, *img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* SAND128 P030 -> R16 / RG16 textures (P010 bits), on GLES: CB1 reads
 * linear planes, the Pi decoder writes columns. Each 128-byte column is
 * imported as its own 32 x colh RGBA8 image (a whole SAND buffer is taller
 * than any texture) and drawn as a strip. The same work the Vulkan engine's
 * sand30_unpack.comp does, so both engines pay it. */
static const char *sand_vs =
    "#version 300 es\n"
    "void main() { vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1); gl_Position = vec4(p * 4.0 - 1.0, 0, 1); }\n";
static const char *sand_fs =
    "#version 300 es\n"
    "precision highp float; precision highp int; precision highp usampler2D;\n"
    "uniform highp sampler2D column;\n"   /* RGBA8: bytes of one 128-byte column */
    "uniform int x0, row0, chroma;\n"     /* first sample of the strip, first row */
    "layout(location = 0) out vec4 o;\n"
    /* n / 3 without an integer divide (V3D has none): exact for n < 2^20,
     * the fraction of (n + 0.5) / 3 being 1/6, 1/2 or 5/6. */
    "int third(int n) { return int((float(n) + 0.5) * (1.0 / 3.0)); }\n"
    "uint word_at(int q, int y) {\n"
    "  vec4 b = texelFetch(column, ivec2(q & 31, row0 + y), 0) * 255.0 + 0.5;\n"
    "  return uint(b.r) | uint(b.g) << 8 | uint(b.b) << 16 | uint(b.a) << 24; }\n"
    "float s10(int n, int y) { int q = third(n);\n"
    "  return float(((word_at(q, y) >> uint((n - 3 * q) * 10)) & 1023u) << 6) / 65535.0; }\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  if (chroma == 0) { o = vec4(s10(p.x - x0, p.y), 0, 0, 1); return; }\n"
    "  int n = 2 * (p.x - x0); o = vec4(s10(n, p.y), s10(n + 1, p.y), 0, 1); }\n";

static GLuint sand_program;
static GLint sand_u_x0, sand_u_row0, sand_u_chroma;

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "cb1 sand shader: %s\n", log);
    }
    return s;
}

static int sand_init(void)
{
    sand_program = glCreateProgram();
    glAttachShader(sand_program, compile(GL_VERTEX_SHADER, sand_vs));
    glAttachShader(sand_program, compile(GL_FRAGMENT_SHADER, sand_fs));
    glLinkProgram(sand_program);
    GLint ok;
    glGetProgramiv(sand_program, GL_LINK_STATUS, &ok);
    sand_u_x0 = glGetUniformLocation(sand_program, "x0");
    sand_u_row0 = glGetUniformLocation(sand_program, "row0");
    sand_u_chroma = glGetUniformLocation(sand_program, "chroma");
    return ok ? 0 : -1;
}

/* One decoded buffer as CB1 sees it, kept per buffer as Kodi keeps its imports. */
struct cb1_input {
    uint64_t ino;
    int w, h;
    struct dvbridge_dmabuf dmabuf;  /* linear P010: CB1's own EGL import */
    AVFrame *mapping;
    /* SAND: per plane, per-column images, and the planes they're unpacked into */
    int sand, columns;
    EGLImageKHR column_image[2][64];
    GLuint column_tex[2][64];
    GLuint planes_tex[2], fbo[2];
    struct dvbridge_gl_frame frame;
};
static struct cb1_input cb1_inputs[256];
static int cb1_input_count;

static struct cb1_input *cb1_input(pl_gpu gpu, const AVFrame *f)
{
    const AVDRMFrameDescriptor *d = dv_drm(f);
    uint64_t ino = inode_of(d->objects[0].fd);
    for (int i = 0; i < cb1_input_count; i++)
        if (cb1_inputs[i].ino == ino && cb1_inputs[i].w == f->width && cb1_inputs[i].h == f->height)
            return &cb1_inputs[i];
    if (cb1_input_count == 256)
        return NULL;
    struct cb1_input *c = &cb1_inputs[cb1_input_count];
    memset(c, 0, sizeof(*c));
    c->ino = ino;
    c->w = f->width;
    c->h = f->height;
    const AVDRMLayerDescriptor *l = &d->layers[0];
    struct dvbridge_gl_plane planes[3] = {0};
    if (dv_is_sand128(d->objects[0].format_modifier)) {
        if (l->format != DRM_FORMAT_P030 || d->nb_objects > 2)
            return NULL;
        c->sand = 1;
        c->columns = (f->width + 95) / 96;
        if (c->columns > 64)
            return NULL;
        /* Column height: the modifier's (both planes on one object, chroma
         * below luma in each column), or 0 and each plane's pitch (V4L2
         * *_COL128M, one object per plane). Each plane's column i is an image
         * from its first row, as tall as the rows left in that column. */
        uint32_t param = dv_sand_height(d->objects[0].format_modifier);
        for (int p = 0; p < 2; p++) {
            const AVDRMPlaneDescriptor *pl = &l->planes[p];
            const AVDRMObjectDescriptor *obj = &d->objects[pl->object_index];
            uint32_t colh = param ? param : (uint32_t)pl->pitch, first = (uint32_t)(pl->offset / 128);
            uint32_t rows = p ? (uint32_t)f->height / 2 : (uint32_t)f->height;
            if (!colh || first + rows > colh)
                return NULL;
            for (int i = 0; i < c->columns; i++) {
                c->column_tex[p][i] = dmabuf_texture(obj->fd, DRM_FORMAT_ABGR8888, 32, (int)(colh - first),
                                                     (uint32_t)pl->offset + (uint32_t)i * 128u * colh, 128,
                                                     &c->column_image[p][i]);
                if (!c->column_tex[p][i]) {
                    fprintf(stderr, "cb1: SAND column import failed (0x%x)\n", eglGetError());
                    return NULL;
                }
            }
        }
        const GLenum fmt[2] = {0x822A /* R16 */, 0x822C /* RG16 */};
        glGenTextures(2, c->planes_tex);
        glGenFramebuffers(2, c->fbo);
        for (int p = 0; p < 2; p++) {
            int pw = p ? f->width / 2 : f->width, ph = p ? f->height / 2 : f->height;
            glBindTexture(GL_TEXTURE_2D, c->planes_tex[p]);
            glTexStorage2D(GL_TEXTURE_2D, 1, fmt[p], pw, ph);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, c->fbo[p]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->planes_tex[p], 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "cb1: R16/RG16 not renderable (EXT_texture_norm16)\n");
                return NULL;
            }
            planes[p] = (struct dvbridge_gl_plane){c->planes_tex[p], pw, ph};
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        /* Linear P010: CB1's own import, from the descriptor VAAPI would map
         * (two layers: R16 luma, GR1616 chroma). */
        if (l->format != DRM_FORMAT_P010)
            return NULL;
        c->mapping = av_frame_alloc();
        AVDRMFrameDescriptor *m = av_mallocz(sizeof(*m));
        m->nb_objects = d->nb_objects;
        memcpy(m->objects, d->objects, sizeof(m->objects));
        m->nb_layers = 2;
        m->layers[0] = (AVDRMLayerDescriptor){.format = DRM_FORMAT_R16, .nb_planes = 1, .planes = {l->planes[0]}};
        m->layers[1] = (AVDRMLayerDescriptor){.format = DRM_FORMAT_GR1616, .nb_planes = 1, .planes = {l->planes[1]}};
        c->mapping->format = AV_PIX_FMT_DRM_PRIME;
        c->mapping->data[0] = (uint8_t *)m;
        c->mapping->buf[0] = av_buffer_create((uint8_t *)m, sizeof(*m), av_buffer_default_free, NULL, 0);
        c->dmabuf = (struct dvbridge_dmabuf){.display = egl_dpy, .mapping = c->mapping, .layout = DVBRIDGE_P010,
                                             .width = f->width, .height = f->height};
        if (!dvbridge_dmabuf_import_mapped(&c->dmabuf))
            return NULL;
        planes[0] = c->dmabuf.planes[0];
        planes[1] = c->dmabuf.planes[1];
    }
    if (!dvbridge_gl_frame_import(&c->frame, gpu, DVBRIDGE_P010, f->width, f->height, planes))
        return NULL;
    cb1_input_count++;
    return c;
}

/* SAND pictures: unpack this picture's columns into the planes (per frame:
 * the decoder rewrites the buffer). */
static void cb1_unpack(struct cb1_input *c)
{
    if (!c->sand)
        return;
    glUseProgram(sand_program);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    for (int p = 0; p < 2; p++) {
        int pw = p ? c->w / 2 : c->w, ph = p ? c->h / 2 : c->h, per = p ? 48 : 96;
        glBindFramebuffer(GL_FRAMEBUFFER, c->fbo[p]);
        glViewport(0, 0, pw, ph);
        glUniform1i(sand_u_chroma, p);
        glUniform1i(sand_u_row0, 0);
        for (int i = 0; i < c->columns; i++) {
            int x0 = i * per;
            if (x0 >= pw)
                break;
            glBindTexture(GL_TEXTURE_2D, c->column_tex[p][i]);
            glUniform1i(sand_u_x0, x0);
            glScissor(x0, 0, per, ph);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

/* ---- statistics ---------------------------------------------------------- */

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

/* ---- one picture, per engine ------------------------------------------------ */

static pl_opengl g_gl;
static struct dvbridge_renderer *g_cb1;
static pl_tex g_target;
static dvvk_engine *g_vke;
static int g_profile, g_fel = 1, g_contract = 1;
/* The Vulkan engine's FEL EL upscale: 1 Annex B (Master Reference) or 2
 * bilinear (the cheaper compromise). */
static unsigned g_el_scaler = 1;
/* CB1: 0 Master Reference (bit-exact, its exact EL filter), 1 Reference
 * (CB1's performance mode, dvbridge_render_packed) with g_cb1_fel as its FEL
 * EL filter (DVBRIDGE_FEL_DEFAULT / BILINEAR / REFERENCE). */
static int g_cb1_mode;
static enum dvbridge_fel_filter g_cb1_fel = DVBRIDGE_FEL_DEFAULT;
/* Per-picture mode: time CB1's SAND unpack on its own (GPU finished around it). */
static int g_time_unpack;
static double g_unpack_ms;

/* CB1 renders picture q (Master Reference) into its target; GL work queued,
 * not finished. 1 on success. */
static int cb1_render(const struct dv_picture *q, double pts)
{
    AVFrame *el = g_fel ? q->el : NULL;
    unsigned w = (unsigned)q->bl->width, h = (unsigned)q->bl->height;
    unsigned x = ((W - w) / 2) & ~1u, y = ((H - h) / 2) & ~1u;
    struct cb1_input *bi = cb1_input(g_gl->gpu, q->bl), *ei = el ? cb1_input(g_gl->gpu, el) : NULL;
    if (!bi || (el && !ei))
        return 0;
    double u0 = 0;
    if (g_time_unpack && bi->sand) {
        glFinish();
        u0 = now_ms();
    }
    cb1_unpack(bi);
    if (ei)
        cb1_unpack(ei);
    if (g_time_unpack && bi->sand) {
        glFinish();
        g_unpack_ms = now_ms() - u0;
    }
    struct dvbridge_gl_frame bl = bi->frame, elf = {0};
    dvbridge_gl_frame_set_chroma(&bl, cb1_chroma(q->bl->chroma_location, g_profile));
    bl.frame.enhancement_layer = NULL;
    if (ei) {
        elf = ei->frame;
        dvbridge_gl_frame_set_chroma(&elf, cb1_chroma(el->chroma_location, 7));
        bl.frame.enhancement_layer = &elf.frame;
    }
    struct dvbridge_geometry g = {.source_width = (int)w, .source_height = (int)h, .x = (int)x, .y = (int)y,
                                  .width = (int)w, .height = (int)h, .target_width = W, .target_height = H};
    dvbridge_renderer_set_optimized_dv(g_cb1, true);
    dvbridge_renderer_set_processing_optimizations(g_cb1, false);
    dvbridge_renderer_set_overlay_visible(g_cb1, false);
    int ok;
    if (g_cb1_mode == 1) {
        while (glGetError() != GL_NO_ERROR)
            ;   /* the packed path refuses to start with a pending GL error */
        dvbridge_renderer_set_fel_filter(g_cb1, g_cb1_fel);
        ok = dvbridge_render_packed(g_cb1, &bl.frame, q->dovi->data, q->dovi->size, pts, el ? pts : NAN, g,
                                    g_target, false);
    } else
        ok = dvbridge_render_master_reference(g_cb1, &bl.frame, q->dovi->data, q->dovi->size, pts,
                                              el ? pts : NAN, g, g_target, false, NULL) == CB1_MASTER_READY;
    if (ok)
        dvbridge_render_commit(g_cb1);
    else
        dvbridge_renderer_reset(g_cb1);
    return ok;
}

/* The Vulkan engine submits picture q (index i) into output `slot`; 1 on
 * success, *ticket to wait on. */
static int vk_submit(const struct dv_picture *q, double pts, long i, unsigned slot, uint64_t *ticket)
{
    AVFrame *el = g_fel ? q->el : NULL;
    unsigned w = (unsigned)q->bl->width, h = (unsigned)q->bl->height;
    unsigned x = ((W - w) / 2) & ~1u, y = ((H - h) / 2) & ~1u;
    uint32_t bl_id = surface_id(q->bl), el_id = el ? surface_id(el) : 0xffffffffu;
    if (vk_register(g_vke, q->bl, bl_id) || (el && vk_register(g_vke, el, el_id)))
        return 0;
    int top_left = cb1_chroma(q->bl->chroma_location, g_profile) == PL_CHROMA_TOP_LEFT;
    dvvk_frame f = {.plan = {.width = w, .height = h, .x = x, .y = y, .dw = w, .dh = h,
                             .chroma_top_left = (uint32_t)top_left, .has_el = el != NULL, .el_scaler = g_el_scaler,
                             .fel = (uint32_t)g_fel, .packet_id = 0, .refresh = i == 0,
                             .pts = (int64_t)(pts * 1e6), .white_nits = 203.0, .cb1_contract = (uint32_t)g_contract},
                    .bl_surface = bl_id, .el_surface = el_id, .metadata = q->dovi->data,
                    .metadata_size = q->dovi->size, .output = slot};
    int rc = vk.submit(g_vke, &f, ticket);
    static int reported;
    if (rc && !reported++)
        fprintf(stderr, "vk: dvvk_submit returned %d (-1 invalid, -2 unsupported by the planner, -8 backend)\n", rc);
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

static int play(const char *file, struct dv_source_options so, int engine, double seconds, struct play_result *res)
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

    /* In flight: the picture each output slot shows, and its completion. */
    struct dv_picture held[3] = {{0}};
    uint64_t tickets[3] = {0};
    GLsync fences[3] = {0};
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
            if (engine == 0) {
                glClientWaitSync(fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
                glDeleteSync(fences[slot]);
            } else
                vk.wait(g_vke, tickets[slot], UINT64_MAX);
            dv_picture_free(&held[slot]);
            pending[slot] = 0;
        }
        /* Deadline check of the previous picture: due at this tick. */
        unsigned prev = (unsigned)((k + 2) % 3);
        if (k > 0 && pending[prev]) {
            int done = engine == 0 ? glClientWaitSync(fences[prev], GL_SYNC_FLUSH_COMMANDS_BIT, 0) != GL_TIMEOUT_EXPIRED
                                   : vk.wait(g_vke, tickets[prev], 0) == 1;
            if (!done) {
                res->late++;
                if (engine == 0)
                    glClientWaitSync(fences[prev], GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
                else
                    vk.wait(g_vke, tickets[prev], UINT64_MAX);
                double l = now_ms() - due[prev];
                if (l > res->worst_late_ms)
                    res->worst_late_ms = l;
            }
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

        double pts = k * period / 1e3;
        int ok;
        if (engine == 0) {
            ok = cb1_render(&p, pts);
            fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
        } else
            ok = vk_submit(&p, pts, k, slot, &tickets[slot]);
        if (!ok) {
            res->failed++;
            if (engine == 0 && fences[slot])
                glDeleteSync(fences[slot]);
            dv_picture_free(&p);
            continue;
        }
        held[slot] = p;
        pending[slot] = 1;
        due[slot] = tick + period;
        res->shown++;
    }
    for (int s = 0; s < 3; s++)
        if (pending[s]) {
            if (engine == 0) {
                glClientWaitSync(fences[s], GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
                glDeleteSync(fences[s]);
            } else
                vk.wait(g_vke, tickets[s], UINT64_MAX);
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

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int frames = 48, warmup = 8, fel = 1, contract = 1, hash = 0, run_cb1 = 1, run_vk = 1;
    double throughput = 0, play_seconds = 0;
    struct dv_source_options so = {.kind = DV_SOURCE_SW, .layout = DV_LAYOUT_SAND30, .extra_frames = 8};
    const char *file = NULL, *node = NULL, *dump = NULL, *json = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--source") && v)
            so.kind = !strcmp(argv[++i], "v4l2") ? DV_SOURCE_V4L2 : DV_SOURCE_SW;
        else if (!strcmp(a, "--layout") && v)
            so.layout = !strcmp(argv[++i], "p010") ? DV_LAYOUT_P010 : DV_LAYOUT_SAND30;
        else if (!strcmp(a, "--el-sw"))
            so.el_software = 1;
        else if (!strcmp(a, "--engines") && v) {
            run_cb1 = strstr(v, "cb1") != NULL;
            run_vk = strstr(v, "vk") != NULL;
            i++;
        } else if (!strcmp(a, "--frames") && v)
            frames = atoi(argv[++i]);
        else if (!strcmp(a, "--warmup") && v)
            warmup = atoi(argv[++i]);
        else if (!strcmp(a, "--start") && v)
            so.start = atof(argv[++i]);
        else if (!strcmp(a, "--fel") && v)
            fel = atoi(argv[++i]);
        else if (!strcmp(a, "--yblod"))
            contract = 0;
        else if (!strcmp(a, "--cb1-mode") && v)
            g_cb1_mode = !strcmp(argv[++i], "reference");
        else if (!strcmp(a, "--cb1-fel") && v) {
            const char *e = argv[++i];
            g_cb1_fel = !strcmp(e, "bilinear") ? DVBRIDGE_FEL_BILINEAR
                      : !strcmp(e, "reference") ? DVBRIDGE_FEL_REFERENCE : DVBRIDGE_FEL_DEFAULT;
        } else if (!strcmp(a, "--el-scaler") && v) {
            const char *e = argv[++i];
            g_el_scaler = !strcmp(e, "bilinear") ? 2 : 1;
        }
        else if (!strcmp(a, "--throughput") && v)
            throughput = atof(argv[++i]);
        else if (!strcmp(a, "--play") && v)
            play_seconds = atof(argv[++i]);
        else if (!strcmp(a, "--node") && v)
            node = argv[++i];
        else if (!strcmp(a, "--hash"))
            hash = 1;
        else if (!strcmp(a, "--dump") && v)
            dump = argv[++i];
        else if (!strcmp(a, "--json") && v)
            json = argv[++i];
        else if (a[0] != '-')
            file = a;
        else {
            fprintf(stderr, "dvbench: unknown option %s\n", a);
            return 2;
        }
    }
    if (!file) {
        fprintf(stderr, "usage: dvbench [--source sw|v4l2] [--layout p010|sand] [--engines cb1,vk] [--frames N] "
                        "[--throughput S] [--hash] [--json FILE] FILE\n");
        return 2;
    }
    /* The Pi's HEVC decoder has at most 32 capture buffers (Raspberry Pi OS
     * kernel) and the bench holds every preloaded picture: keep it under. */
    if (so.kind == DV_SOURCE_V4L2 && frames > 12) {
        fprintf(stderr, "dvbench: --frames %d -> 12 (the hardware decoder's buffer pool)\n", frames);
        frames = 12;
    }
    if (warmup >= frames)
        warmup = frames > 4 ? 2 : 0;
    /* CB1's SAND input holds an EGL image (an fd) per 128-byte column, per
     * plane, per decoded buffer: thousands, beyond the usual 1024. */
    struct rlimit nofile;
    if (!getrlimit(RLIMIT_NOFILE, &nofile) && nofile.rlim_cur < nofile.rlim_max) {
        nofile.rlim_cur = nofile.rlim_max;
        setrlimit(RLIMIT_NOFILE, &nofile);
    }
    so.extra_frames = so.kind == DV_SOURCE_V4L2 ? frames + 2 : frames + 8;

    /* 1. Decode: every picture first, timed, held for the engines. */
    struct dv_source *src = dv_source_open(file, &so);
    if (!src)
        return 1;
    int profile = dv_source_profile(src);
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
    const AVDRMFrameDescriptor *dd = dv_drm(b0);
    printf("source: %s, profile %d, %dx%d%s, %s %s\n", file, profile, b0->width, b0->height,
           pics[0].el ? " + EL" : "", so.kind == DV_SOURCE_V4L2 ? "v4l2 request" : "software decode",
           dv_is_sand128(dd->objects[0].format_modifier) ? "SAND128 P030" : "linear P010");
    printf("decode: %d pictures in %.0f ms, %.1f fps (CPU %.1f ms/picture%s)\n", count, decode_ms,
           count * 1e3 / decode_ms, decode_cpu / count,
           so.kind == DV_SOURCE_SW ? ", includes the copy into dmabufs" : "");
    if (so.kind == DV_SOURCE_SW)
        printf("decode: of that, %.1f ms/picture copying into %s dmabufs\n", copy_ms / count,
               so.layout == DV_LAYOUT_SAND30 ? "SAND128" : "P010");

    AVRational rate = dv_source_rate(src);
    double period = rate.num > 0 ? (double)rate.den / rate.num : 1.0 / 24;

    /* 2. Engines. */
    dvvk_engine *vke = NULL;
    struct out_buf vk_out[3], cb_out;
    memset(vk_out, 0, sizeof(vk_out));
    memset(&cb_out, 0, sizeof(cb_out));
    if (run_vk) {
        int drm = node ? open(node, O_RDWR | O_CLOEXEC) : -1;
        if (load_vk() || vk.create(drm, NULL, &vke)) {
            fprintf(stderr, "vk: engine didn't start\n");
            return 1;
        }
        for (int i = 0; i < 3; i++)
            if (out_alloc(&vk_out[i]) || vk.set_output(vke, (uint32_t)i, vk_out[i].fd, vk_out[i].stride)) {
                fprintf(stderr, "vk: output %d failed\n", i);
                return 1;
            }
    }
    pl_opengl gl = NULL;
    struct dvbridge_renderer *cb1 = NULL;
    pl_tex target = NULL;
    if (run_cb1) {
        egl_dpy = ((PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT"))(
            EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (!egl_dpy || !eglInitialize(egl_dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
            fprintf(stderr, "cb1: EGL init failed\n");
            return 1;
        }
        const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
        EGLContext ctx = eglCreateContext(egl_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
        if (!ctx || !eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            fprintf(stderr, "cb1: EGL context failed\n");
            return 1;
        }
        egl_create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
        egl_destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
        gl_image_target = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
        pl_log log = pl_log_create(PL_API_VER, pl_log_params(.log_cb = pl_log_color, .log_level = PL_LOG_WARN));
        gl = pl_opengl_create(log, pl_opengl_params(.get_proc_addr = (pl_voidfunc_t (*)(const char *))eglGetProcAddress,
                                                    .egl_display = egl_dpy, .egl_context = ctx,
                                                    .allow_software = true));
        struct dvbridge_retirement_owner *owner = gl ? dvbridge_retirement_owner_create(gl->gpu) : NULL;
        cb1 = owner ? dvbridge_renderer_create_with_owner(owner) : NULL;
        if (!cb1 || !dvbridge_renderer_supports_mode(cb1, DVBRIDGE_MODE_MASTER_REFERENCE)) {
            fprintf(stderr, "cb1: renderer unavailable or without Master Reference on this GPU\n");
            return 1;
        }
        printf("cb1: GL %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        EGLImageKHR img;
        if (out_alloc(&cb_out) || sand_init()) {
            fprintf(stderr, "cb1: output or SAND shader failed\n");
            return 1;
        }
        GLuint tex = dmabuf_texture(cb_out.fd, DRM_FORMAT_ARGB8888, W, H, 0, cb_out.stride, &img);
        target = tex ? pl_opengl_wrap(gl->gpu, pl_opengl_wrap_params(.texture = tex, .width = W, .height = H,
                                                                      .iformat = GL_RGBA8)) : NULL;
        if (!target) {
            fprintf(stderr, "cb1: output import failed\n");
            return 1;
        }
    }

    g_gl = gl;
    g_cb1 = cb1;
    g_target = target;
    g_vke = vke;
    g_profile = profile;
    g_fel = fel;
    g_contract = contract;

    /* 3. Per picture, synchronous. */
    FILE *js = json ? fopen(json, "w") : NULL;
    if (js)
        fprintf(js, "{\"file\": \"%s\", \"profile\": %d, \"width\": %d, \"height\": %d, \"el\": %s, "
                    "\"source\": \"%s\", \"layout\": \"%s\", \"pictures\": %d, \"decode_fps\": %.2f, "
                    "\"decode_cpu_ms\": %.2f, \"copy_ms\": %.2f, \"engines\": {",
                file, profile, b0->width, b0->height, pics[0].el ? "true" : "false",
                so.kind == DV_SOURCE_V4L2 ? "v4l2" : "sw",
                dv_is_sand128(dd->objects[0].format_modifier) ? "sand128" : "p010", count, count * 1e3 / decode_ms,
                decode_cpu / count, copy_ms / count);
    int status = 0, first_engine = 1;
    double *times = calloc((size_t)count, sizeof(double));
    for (int engine = 0; engine < 2; engine++) {
        if ((engine == 0 && !run_cb1) || (engine == 1 && !run_vk))
            continue;
        const char *name = engine == 0 ? "cb1" : "yblod-vulkan";
        int timed = 0, failed = 0, unpacks = 0;
        double *unpack_times = calloc((size_t)count, sizeof(double));
        for (int i = 0; i < count; i++) {
            struct dv_picture *q = &pics[i];
            double pts = i * period;
            int ok = 0;
            const struct out_buf *out = engine == 0 ? &cb_out : &vk_out[0];
            double t0 = now_ms();
            if (engine == 0) {
                g_time_unpack = 1;
                g_unpack_ms = -1;
                ok = cb1_render(q, pts);
                g_time_unpack = 0;
                glFinish();
                if (ok && i >= warmup && g_unpack_ms >= 0)
                    unpack_times[unpacks++] = g_unpack_ms;
            } else {
                uint64_t ticket = 0;
                ok = vk_submit(q, pts, i, 0, &ticket) && vk.wait(vke, ticket, 10000000000ull) == 1;
            }
            double t = now_ms() - t0;
            if (!ok) {
                failed++;
                continue;
            }
            if (i >= warmup)
                times[timed++] = t;
            if (hash)
                printf("hash %s %03d %016llx\n", name, i, (unsigned long long)out_hash(out));
            if (dump)
                out_dump(out, dump, name, i);
        }
        struct stats s = summarize(times, timed);
        printf("%-13s per picture: median %.2f ms, p95 %.2f, mean %.2f (%d pictures%s), %.1f fps%s\n", name, s.median,
               s.p95, s.mean, s.n, warmup ? " after warmup" : "", s.median > 0 ? 1e3 / s.median : 0,
               failed ? " FAILURES" : "");
        struct stats us = summarize(unpack_times, unpacks);
        if (unpacks)
            printf("%-13s of which the bench's SAND->P010 GLES pass: median %.2f ms (CB1 itself: %.2f ms)\n", name,
                   us.median, s.median - us.median);
        free(unpack_times);
        if (failed) {
            printf("%-13s %d pictures failed\n", name, failed);
            status = 1;
        }
        if (js) {
            fprintf(js, "%s\"%s\": {\"median_ms\": %.3f, \"p95_ms\": %.3f, \"mean_ms\": %.3f, \"timed\": %d, "
                        "\"failed\": %d",
                    first_engine ? "" : ", ", name, s.median, s.p95, s.mean, s.n, failed);
            if (unpacks)
                fprintf(js, ", \"sand_unpack_ms\": %.3f", us.median);
            first_engine = 0;
        }

        /* 4. Throughput: flat out, pipelined as in playback (Vulkan: three
         * frames in flight; CB1: GL queued, finished at the end). */
        if (throughput > 0) {
            uint64_t tickets[3] = {0};
            long done = 0, tfail = 0;
            double t0 = now_ms(), c0 = cpu_ms(), end = t0 + throughput * 1e3;
            for (long i = 0; now_ms() < end; i++) {
                struct dv_picture *q = &pics[i % count];
                double pts = i * period;
                int ok = 0;
                if (engine == 0) {
                    ok = cb1_render(q, pts);
                } else {
                    unsigned slot = (unsigned)(i % 3);
                    if (tickets[slot] && vk.wait(vke, tickets[slot], UINT64_MAX) != 1)
                        tfail++;
                    tickets[slot] = 0;
                    ok = vk_submit(q, pts, i, slot, &tickets[slot]);
                }
                done += ok;
                tfail += !ok;
            }
            if (engine == 0)
                glFinish();
            else
                for (int k = 0; k < 3; k++)
                    if (tickets[k])
                        vk.wait(vke, tickets[k], UINT64_MAX);
            double wall = now_ms() - t0, cpu = cpu_ms() - c0;
            printf("%-13s throughput: %.1f fps over %.1f s, CPU %.2f ms/picture%s\n", name, done * 1e3 / wall,
                   wall / 1e3, done ? cpu / done : 0, tfail ? " FAILURES" : "");
            if (js)
                fprintf(js, ", \"throughput_fps\": %.2f, \"throughput_cpu_ms\": %.3f", done * 1e3 / wall,
                        done ? cpu / done : 0);
            if (tfail)
                status = 1;
        }

        /* 5. Real-time playback from a decoding thread. */
        if (play_seconds > 0) {
            struct play_result r;
            if (play(file, so, engine, play_seconds, &r)) {
                printf("%-13s play: source failed\n", name);
                status = 1;
            } else {
                printf("%-13s play: %ld pictures in %.1f s at the stream rate, %ld late (worst %.1f ms), "
                       "%ld decode stalls, CPU %.2f ms/picture%s\n",
                       name, r.shown, r.wall_ms / 1e3, r.late, r.worst_late_ms, r.starved,
                       r.shown ? r.cpu_ms / r.shown : 0, r.failed ? " FAILURES" : "");
                if (js)
                    fprintf(js, ", \"play_shown\": %ld, \"play_late\": %ld, \"play_worst_late_ms\": %.2f, "
                                "\"play_decode_stalls\": %ld, \"play_cpu_ms\": %.3f",
                            r.shown, r.late, r.worst_late_ms, r.starved, r.shown ? r.cpu_ms / r.shown : 0);
                if (r.failed)
                    status = 1;
            }
        }
        if (js)
            fprintf(js, "}");
    }
    if (js) {
        fprintf(js, "}}\n");
        fclose(js);
    }
    for (int i = 0; i < count; i++)
        dv_picture_free(&pics[i]);
    if (vke && vk.destroy)
        vk.destroy(vke);
    dv_source_close(src);
    return status;
}
