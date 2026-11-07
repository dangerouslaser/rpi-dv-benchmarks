/* The Pi engine (pi_engine.h): two fragment passes per frame, each one render
 * pass (one V3D render job), reading the decoder's SAND128 dmabufs directly
 * as texel buffers and rendering the tunnel frame into the linear output
 * dmabuf itself.
 *   P1  cw x ch   reconstructed 4:2:0 chroma (RG16UI) + the EL luma (R16UI)
 *   P2  cw x ch   the 4:2:2 filter + luma + pack + metadata, a 2x2 block per
 *                 fragment -> the output's even and odd rows (two RG32UI
 *                 images over the same dmabuf)
 * Per frame: plan (CPU), the tables (only when the planner's generation
 * changed), the pipelines (cached; the scene's family built ahead on a
 * worker thread), then record and submit.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pi_engine.h"
#include "pi_internal.h"

#include <errno.h>
#include <libavutil/hwcontext_drm.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "dvvk_plan.h"
#include "shaders/pack.h"

static const uint32_t spv_vert[] =
#include "shaders/fullscreen.vert.h"
    ;
static const uint32_t spv_p1[] =
#include "shaders/p1_chroma.frag.h"
    ;
static const uint32_t spv_p2[] =
#include "shaders/p2_pack.frag.h"
    ;

/* ---- layouts shared with the shaders --------------------------------------- */

/* The per-frame tables (common.glsl T_*), 32-bit words: luma mapping (+8),
 * residuals, polynomial chroma, float MMR LUTs, 64-bit MMR LUTs, single-layer
 * tables, the 128-bit MMR parameters (2 channels x 22 x 4 words). */
enum { T_MAP = 0, T_RES = 1024, T_CMAP = 4096, T_LUTF = 6144, T_LUT64 = 12288, T_SINGLE = 24576, T_WIDE = 27648 };
enum { WIDE_WORDS = 22 * 4, T_WORDS = T_WIDE + 2 * WIDE_WORDS };
_Static_assert(T_WORDS % 4 == 0, "the tables are also an RGBA32UI buffer");

struct params {   /* common.glsl Params, std140 */
    uint32_t size[4], bl[4], el[4], el_size[4], active[4], black[4], pk[4];
    uint32_t lo[2][4], hi[2][4], const64[2][4];
    int32_t c32[6][4];
    float cf[6][4], cconst[4], inv[4], cs[6][4], cy[4];
    uint32_t narrow[4], pw[4];
    float err[4];
    uint32_t packets[160];
};
_Static_assert(sizeof(struct params) == 1232, "common.glsl Params");
_Static_assert(offsetof(struct params, c32) == 208, "common.glsl Params");
_Static_assert(offsetof(struct params, packets) == 592, "common.glsl Params");
_Static_assert(sizeof(((struct params *)0)->packets) == sizeof(((dvvk_plan *)0)->packets), "packets");

/* Descriptor bindings (common.glsl, p1_chroma.frag, p2_pack.frag, piecewise.glsl). */
static const struct {
    uint32_t binding;
    VkDescriptorType type;
} bindings[] = {
    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},          /* Params */
    {1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},    /* tables, R32UI */
    {2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},    /* tables, RG32UI */
    {3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},    /* BL luma */
    {4, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},    /* BL chroma */
    {5, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},    /* EL luma */
    {6, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},    /* EL chroma */
    {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},  /* P1's chroma */
    {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},  /* P1's EL luma */
    {10, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER},   /* tables, RGBA32UI */
    {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},         /* route 3: dv_single_reshape */
};
enum { NB = sizeof(bindings) / sizeof(bindings[0]) };

/* Specialisation keys: P1's constant ids 0..12, P2's 1..6. */
enum { K1_MODE, K1_METHOD, K1_DUAL, K1_KIND0, K1_KIND1, K1_ORDER0, K1_ORDER1, K1_SINGLE, K1_PIECE, K1_ORDER2,
       K1_FAST, K1_MARK, K1_GUARD, K1_N };
enum { K2_ANNEXB, K2_TOP_LEFT, K2_ABLATE, K2_BARS, K2_SINGLE, K2_FULL422, K2_N };
enum { MAX_SPEC = 16, KEY_MAX = K1_N };
_Static_assert((int)K1_N <= (int)MAX_SPEC && (int)K2_N + 1 <= (int)MAX_SPEC, "a specialisation map fits its entries");

/* yblod's dv_single_reshape (single_reshape.h), as the planner fills
 * dvvk_plan.reshape. */
struct reshape_piece {
    uint32_t method, order;
    int64_t constant, coefficient[3][7];
};
struct reshape_curve {
    uint32_t pivot_count, pivots[9];
    struct reshape_piece piece[8];
};
struct reshape {
    uint32_t base_depth, denominator;
    struct reshape_curve curve[3];
};
_Static_assert(sizeof(struct reshape) == sizeof(((dvvk_plan *)0)->reshape), "dv_single_reshape layout");

/* ---- the engine ------------------------------------------------------------- */

struct image {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
};

/* A decoder dmabuf, imported once per stream (by inode: stable while the
 * import holds the buffer). */
struct import {
    dev_t dev;
    ino_t ino;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkBufferView view;
    uint64_t last_used;   /* the render serial that last read it */
    int refs;             /* in-flight renders reading it */
    struct import *next;
};

struct pi_output {
    uint32_t w, h, stride;
    struct image half[2];   /* the even, odd rows: the dmabuf itself, or copy sources */
    VkBuffer copy_buf;      /* copy mode: the dmabuf as a buffer */
    VkDeviceMemory copy_mem;
    VkFramebuffer fb;
    int ctx;                /* its last render's context, and that render's serial (0: none) */
    uint64_t serial;
    struct pi_render_stats stats;
    struct pi_output *next;
};

/* A frame in flight. */
struct ctx {
    struct image c420, ely;
    VkFramebuffer fb;
    VkBuffer tables, ubo, reshape;
    VkDeviceMemory tables_mem, ubo_mem, reshape_mem;
    uint32_t *tmap;
    struct params *pmap;
    void *rmap;
    uint64_t tables_serial;   /* e->tables_serial its tables hold, 0 none */
    VkBufferView tview, tview64, tview128;
    VkDescriptorSet ds;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquire, release;   /* sync_file import / export, where supported */
    VkQueryPool qp;
    int busy;
    uint64_t serial;
    AVFrame *bl, *el;
    struct import *in[4];
    struct pi_output *out;
    struct pi_render_stats stats;
};

struct variant {
    uint32_t key[KEY_MAX];
    VkPipeline p;
};
struct variants {
    struct variant *v;
    int n, cap;
};

/* What the tables of one planner generation lead to (prepare). */
struct route {
    int single, piece, fast, wide;
    uint32_t mode, guard, order[2];
};

enum {   /* warnings, each once per engine */
    W_PLAN = 1, W_GUARD = 2, W_INPUT = 4, W_PIPELINE = 8, W_IMPORT = 16, W_PLANNER = 32,
};

struct pi_engine {
    struct pi_options o;
    char *cache_path;
    char name[320];
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties mp;
    PFN_vkGetMemoryFdPropertiesKHR get_fd_props;
    PFN_vkImportSemaphoreFdKHR import_sem_fd;
    PFN_vkGetSemaphoreFdKHR get_sem_fd;
    int modifiers, foreign, sem_fd, profile;
    double ts_period;
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkRenderPass rp[2];   /* P1, P2 */
    VkPipelineCache cache;

    pthread_mutex_t lock;   /* the variants and the prewarm queue */
    struct variants v1, v2;
    pthread_t worker;
    int worker_started, worker_stop, nq;
    pthread_cond_t wake;
    uint32_t queue_keys[64][KEY_MAX];

    struct ctx c[PI_FRAMES_IN_FLIGHT];
    unsigned next;
    uint64_t serial;
    struct import *imports;
    int nimports;
    struct pi_output *outputs;

    dvvk_plan_state *state;
    dvvk_plan *plan;
    int open, first;
    uint32_t w, h;
    int profile_dv;

    /* the current tables (prepare): their serial, for the contexts' copies */
    uint32_t *tables;
    struct params tmpl;   /* the Params the tables lead to; per-frame fields filled per frame */
    struct route route;
    uint64_t tables_serial;
    int have_tables;
    uint32_t tables_gen;
    uint32_t sig[16];     /* the plan fields the route depends on */

    unsigned warned;
};

/* ---- logging ------------------------------------------------------------------ */

static void logf_(const struct pi_engine *e, enum pi_log_level level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void logf_(const struct pi_engine *e, enum pi_log_level level, const char *fmt, ...)
{
    if (e && level > e->o.log_level)
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (e && e->o.log)
        e->o.log(e->o.log_opaque, level, msg);
    else
        fprintf(stderr, "pi: %s\n", msg);
}

/* Once per engine for each warning bit. */
#define WARN_ONCE(e, bit, ...)                     \
    do {                                           \
        if (!((e)->warned & (bit))) {              \
            (e)->warned |= (bit);                  \
            logf_((e), PI_LOG_WARNING, __VA_ARGS__); \
        }                                          \
    } while (0)

#define VKC(e, x)                                                                      \
    do {                                                                               \
        VkResult r_ = (x);                                                             \
        if (r_ != VK_SUCCESS) {                                                        \
            logf_((e), PI_LOG_ERROR, "%s:%d: %s = %d", __FILE__, __LINE__, #x, (int)r_); \
            return PI_E_VULKAN;                                                        \
        }                                                                              \
    } while (0)

const char *pi_strerror(int s)
{
    switch (s) {
    case PI_OK: return "OK";
    case PI_E_INVALID: return "invalid argument or call order";
    case PI_E_UNSUPPORTED_FORMAT: return "input isn't a SAND128 DRM PRIME frame of the stream's size";
    case PI_E_UNSUPPORTED_PLAN: return "the planner refuses the stream, or plans outside this engine";
    case PI_E_BAD_RPU: return "invalid RPU";
    case PI_E_VULKAN: return "Vulkan error";
    case PI_E_NOMEM: return "out of memory";
    default: return "unknown status";
    }
}

static const char *mode_name(enum pi_mmr_mode m)
{
    switch (m) {
    case PI_MMR_FLOAT: return "float";
    case PI_MMR_EXACT64: return "exact64 (emulated 64-bit)";
    case PI_MMR_SPLIT: return "split (integer low word + float high word)";
    case PI_MMR_EXACT: return "exact (float + error bound, split where it straddles)";
    }
    return "?";
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- Vulkan objects --------------------------------------------------------- */

/* A memory type with all of want; UINT32_MAX if none. */
static uint32_t memtype(const struct pi_engine *e, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < e->mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (e->mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int allocate(struct pi_engine *e, const VkMemoryRequirements *r, VkMemoryPropertyFlags want, const void *next,
                    VkDeviceMemory *m)
{
    uint32_t type = memtype(e, r->memoryTypeBits, want);
    if (type == UINT32_MAX) {
        logf_(e, PI_LOG_ERROR, "no memory type with flags %#x (types %#x)", (unsigned)want, r->memoryTypeBits);
        return PI_E_VULKAN;
    }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = next,
                               .allocationSize = r->size, .memoryTypeIndex = type};
    VKC(e, vkAllocateMemory(e->dev, &ai, NULL, m));
    return 0;
}

static int host_buffer(struct pi_engine *e, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b,
                       VkDeviceMemory *m, void **map)
{
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VKC(e, vkCreateBuffer(e->dev, &ci, NULL, b));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(e->dev, *b, &r);
    int rc = allocate(e, &r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, NULL, m);
    if (rc)
        return rc;
    VKC(e, vkBindBufferMemory(e->dev, *b, *m, 0));
    VKC(e, vkMapMemory(e->dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return 0;
}

static int view(struct pi_engine *e, struct image *im, VkFormat f)
{
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKC(e, vkCreateImageView(e->dev, &vi, NULL, &im->view));
    return 0;
}

static int image(struct pi_engine *e, struct image *im, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(e, vkCreateImage(e->dev, &ci, NULL, &im->img));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(e->dev, im->img, &r);
    int rc = allocate(e, &r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL, &im->mem);
    if (rc)
        return rc;
    VKC(e, vkBindImageMemory(e->dev, im->img, im->mem, 0));
    return view(e, im, f);
}

static void image_destroy(struct pi_engine *e, struct image *im)
{
    vkDestroyImageView(e->dev, im->view, NULL);
    vkDestroyImage(e->dev, im->img, NULL);
    vkFreeMemory(e->dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static int import_memory(struct pi_engine *e, int fd, const VkMemoryRequirements *r, const void *dedicated,
                         VkDeviceMemory *m)
{
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VKC(e, e->get_fd_props(e->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));
    uint32_t type = memtype(e, r->memoryTypeBits & fp.memoryTypeBits, 0);
    if (type == UINT32_MAX) {
        logf_(e, PI_LOG_ERROR, "no memory type for the dmabuf");
        return PI_E_VULKAN;
    }
    int dupfd = dup(fd);
    if (dupfd < 0)
        return PI_E_INVALID;
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = dedicated,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dupfd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                               .allocationSize = r->size, .memoryTypeIndex = type};
    VkResult res = vkAllocateMemory(e->dev, &ai, NULL, m);
    if (res != VK_SUCCESS) {
        close(dupfd);   /* the import takes it only on success */
        logf_(e, PI_LOG_ERROR, "dmabuf import (%llu bytes) failed: %d", (unsigned long long)r->size, (int)res);
        return PI_E_VULKAN;
    }
    return 0;
}

static off_t dmabuf_size(int fd)
{
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return size;
}

static void import_free(struct pi_engine *e, struct import *i)
{
    vkDestroyBufferView(e->dev, i->view, NULL);
    vkDestroyBuffer(e->dev, i->buf, NULL);
    vkFreeMemory(e->dev, i->mem, NULL);
    free(i);
}

/* A decoder dmabuf as an R32UI texel buffer, once per buffer. */
static struct import *import_dmabuf(struct pi_engine *e, int fd)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st))
        return NULL;
    for (struct import *i = e->imports; i; i = i->next)
        if (i->dev == st.st_dev && i->ino == st.st_ino)
            return i;
    off_t size = dmabuf_size(fd);
    if (size <= 0)
        return NULL;
    struct import *i = calloc(1, sizeof(*i));
    if (!i)
        return NULL;
    i->dev = st.st_dev;
    i->ino = st.st_ino;
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    /* Texel-buffer use only: v3dv pads no read-ahead onto it (as it does
     * for transfer / storage / uniform use), so the requirement is the
     * dmabuf's own size. */
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = (VkDeviceSize)size,
                             .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
    VkMemoryRequirements r;
    if (vkCreateBuffer(e->dev, &ci, NULL, &i->buf) != VK_SUCCESS)
        goto fail;
    vkGetBufferMemoryRequirements(e->dev, i->buf, &r);
    if (r.size > (VkDeviceSize)size) {
        logf_(e, PI_LOG_ERROR, "buffer needs %llu bytes, the dmabuf has %lld", (unsigned long long)r.size,
              (long long)size);
        goto fail;
    }
    if (import_memory(e, fd, &r, NULL, &i->mem) || vkBindBufferMemory(e->dev, i->buf, i->mem, 0) != VK_SUCCESS)
        goto fail;
    VkBufferViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = i->buf,
                                 .format = VK_FORMAT_R32_UINT, .range = VK_WHOLE_SIZE};
    if (vkCreateBufferView(e->dev, &vi, NULL, &i->view) != VK_SUCCESS)
        goto fail;
    i->next = e->imports;
    e->imports = i;
    e->nimports++;
    return i;
fail:
    import_free(e, i);
    return NULL;
}

/* Imports no render has read for a while and none in flight reads: a
 * decoder that reallocates mid-stream would otherwise grow the list. */
static void imports_trim(struct pi_engine *e)
{
    if (e->nimports <= 48)
        return;
    for (struct import **p = &e->imports; *p;) {
        struct import *i = *p;
        if (!i->refs && i->last_used + 64 < e->serial) {
            *p = i->next;
            import_free(e, i);
            e->nimports--;
        } else
            p = &i->next;
    }
}

static void imports_release(struct pi_engine *e)
{
    while (e->imports) {
        struct import *i = e->imports;
        e->imports = i->next;
        import_free(e, i);
    }
    e->nimports = 0;
}

static int render_pass(struct pi_engine *e, int n, const VkFormat *formats, VkImageLayout final, int clear,
                       VkRenderPass *rp)
{
    VkAttachmentDescription a[2];
    VkAttachmentReference ref[2];
    for (int i = 0; i < n; i++) {
        a[i] = (VkAttachmentDescription){.format = formats[i], .samples = VK_SAMPLE_COUNT_1_BIT,
                                         .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final};
        ref[i] = (VkAttachmentReference){(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = (uint32_t)n,
                               .pColorAttachments = ref};
    /* Its output is read by the next pass's fragment shaders (or copied, or
     * read by the host after the fence). */
    VkSubpassDependency dep[2] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT},
        {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_HOST_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT}};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = (uint32_t)n,
                                 .pAttachments = a, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2,
                                 .pDependencies = dep};
    VKC(e, vkCreateRenderPass(e->dev, &ci, NULL, rp));
    return 0;
}

/* ---- pipelines ------------------------------------------------------------------ */

static int pipeline(struct pi_engine *e, const uint32_t *spv, size_t bytes, VkRenderPass rp, const uint32_t *spec,
                    uint32_t nspec, uint32_t first, VkPipeline *p)
{
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = spv};
    VkShaderModule fs = VK_NULL_HANDLE, vs = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(e->dev, &mi, NULL, &fs);
    mi.codeSize = sizeof(spv_vert);
    mi.pCode = spv_vert;
    if (r == VK_SUCCESS)
        r = vkCreateShaderModule(e->dev, &mi, NULL, &vs);
    VkSpecializationMapEntry me[MAX_SPEC];
    for (uint32_t i = 0; i < nspec; i++)
        me[i] = (VkSpecializationMapEntry){first + i, 4 * i, 4};
    VkSpecializationInfo si = {nspec, me, 4 * nspec, spec};
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs, .pName = "main", .pSpecializationInfo = &si}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba[2] = {{.colorWriteMask = 0xf}, {.colorWriteMask = 0xf}};
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 2, .pAttachments = cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cbs, .pDynamicState = &ds, .layout = e->pl,
                                       .renderPass = rp};
    if (r == VK_SUCCESS)
        r = vkCreateGraphicsPipelines(e->dev, e->cache, 1, &ci, NULL, p);
    vkDestroyShaderModule(e->dev, fs, NULL);
    vkDestroyShaderModule(e->dev, vs, NULL);
    if (r != VK_SUCCESS) {
        logf_(e, PI_LOG_ERROR, "pipeline creation failed: %d", (int)r);
        return PI_E_VULKAN;
    }
    return 0;
}

/* The two shaders' variant sets: P1 (keys K1_N, constant ids from 0) and P2
 * (K2_N, from 1). */
static struct variants *set_of(struct pi_engine *e, int p2) { return p2 ? &e->v2 : &e->v1; }
static uint32_t key_n(int p2) { return p2 ? K2_N : K1_N; }

static VkPipeline find_locked(struct variants *s, const uint32_t *key, uint32_t n)
{
    for (int i = 0; i < s->n; i++)
        if (!memcmp(s->v[i].key, key, n * 4))
            return s->v[i].p;
    return VK_NULL_HANDLE;
}

/* Builds the variant `key` if no one has; returns it. Safe from the worker. */
static VkPipeline build(struct pi_engine *e, int p2, const uint32_t *key)
{
    struct variants *s = set_of(e, p2);
    uint32_t n = key_n(p2);
    pthread_mutex_lock(&e->lock);
    VkPipeline p = find_locked(s, key, n);
    pthread_mutex_unlock(&e->lock);
    if (p)
        return p;
    double t0 = now_ms();
    VkPipeline made;
    if (p2 ? pipeline(e, spv_p2, sizeof(spv_p2), e->rp[1], key, n, 1, &made)
           : pipeline(e, spv_p1, sizeof(spv_p1), e->rp[0], key, n, 0, &made))
        return VK_NULL_HANDLE;
    pthread_mutex_lock(&e->lock);
    p = find_locked(s, key, n);   /* the other thread may have made it meanwhile */
    if (!p && s->n == s->cap) {
        int cap = s->cap ? 2 * s->cap : 16;
        struct variant *v = realloc(s->v, (size_t)cap * sizeof(*v));
        if (v) {
            s->v = v;
            s->cap = cap;
        }
    }
    if (!p && s->n < s->cap) {
        memset(s->v[s->n].key, 0, sizeof(s->v[s->n].key));
        memcpy(s->v[s->n].key, key, n * 4);
        s->v[s->n++].p = p = made;
        made = VK_NULL_HANDLE;
    }
    pthread_mutex_unlock(&e->lock);
    if (made)
        vkDestroyPipeline(e->dev, made, NULL);
    logf_(e, PI_LOG_DEBUG, "pipeline %s variant made in %.0f ms", p2 ? "P2" : "P1", now_ms() - t0);
    return p;
}

/* The worker: builds queued P1 variants ahead of the frames that need them. */
static void *worker_main(void *arg)
{
    struct pi_engine *e = arg;
    /* Idle priority: compiling ahead must not take time from the decoder or
     * the render thread. */
    struct sched_param sp = {0};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
    pthread_mutex_lock(&e->lock);
    while (!e->worker_stop) {
        if (!e->nq) {
            pthread_cond_wait(&e->wake, &e->lock);
            continue;
        }
        uint32_t key[KEY_MAX];
        memcpy(key, e->queue_keys[--e->nq], sizeof(key));
        pthread_mutex_unlock(&e->lock);
        build(e, 0, key);
        pthread_mutex_lock(&e->lock);
    }
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

static void prewarm(struct pi_engine *e, const uint32_t *key)
{
    pthread_mutex_lock(&e->lock);
    int known = find_locked(&e->v1, key, K1_N) != VK_NULL_HANDLE;
    for (int i = 0; i < e->nq && !known; i++)
        known = !memcmp(e->queue_keys[i], key, K1_N * 4);
    if (!known && e->nq < (int)(sizeof(e->queue_keys) / sizeof(e->queue_keys[0]))) {
        memcpy(e->queue_keys[e->nq++], key, K1_N * 4);
        pthread_cond_signal(&e->wake);
    }
    pthread_mutex_unlock(&e->lock);
}

/* The P1 variants this stream may need next: its MMR's other orders (they
 * change with the scene) and the split's guard on and off. */
static void prewarm_family(struct pi_engine *e, const uint32_t *k1)
{
    if (!e->worker_started)
        return;
    int mmr = k1[K1_FAST] || ((k1[K1_KIND0] || k1[K1_KIND1]) && !k1[K1_SINGLE] && !k1[K1_PIECE]);
    if (!mmr)
        return;
    int guards = k1[K1_MODE] == PI_MMR_SPLIT || k1[K1_MODE] == PI_MMR_EXACT ? 2 : 1;
    uint32_t key[KEY_MAX];
    memcpy(key, k1, sizeof(key));
    for (uint32_t g = 0; g < (uint32_t)guards; g++)
        for (uint32_t o0 = 1; o0 <= 3; o0++)
            for (uint32_t o1 = 1; o1 <= 3; o1++) {
                key[K1_GUARD] = g;
                key[K1_ORDER0] = o0;
                key[K1_ORDER1] = o1;
                prewarm(e, key);
            }
}

static void worker_stop(struct pi_engine *e)
{
    if (!e->worker_started)
        return;
    pthread_mutex_lock(&e->lock);
    e->worker_stop = 1;
    e->nq = 0;
    pthread_cond_signal(&e->wake);
    pthread_mutex_unlock(&e->lock);
    pthread_join(e->worker, NULL);
    e->worker_started = 0;
}

static void cache_load(struct pi_engine *e)
{
    void *data = NULL;
    size_t size = 0;
    FILE *f = e->cache_path ? fopen(e->cache_path, "rb") : NULL;
    if (f) {
        if (!fseek(f, 0, SEEK_END)) {
            long n = ftell(f);
            if (n > 0 && n < (64 << 20) && (data = malloc((size_t)n)) && !fseek(f, 0, SEEK_SET) &&
                fread(data, 1, (size_t)n, f) == (size_t)n)
                size = (size_t)n;
        }
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                    .initialDataSize = size, .pInitialData = size ? data : NULL};
    /* a stale or foreign cache: the driver ignores it, or refuses it */
    if (vkCreatePipelineCache(e->dev, &ci, NULL, &e->cache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(e->dev, &ci, NULL, &e->cache) != VK_SUCCESS)
            e->cache = VK_NULL_HANDLE;
    }
    free(data);
}

static void cache_save(struct pi_engine *e)
{
    size_t size = 0;
    if (!e->cache || !e->cache_path || vkGetPipelineCacheData(e->dev, e->cache, &size, NULL) != VK_SUCCESS || !size)
        return;
    void *data = malloc(size);
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", e->cache_path);
    FILE *f;
    if (data && vkGetPipelineCacheData(e->dev, e->cache, &size, data) == VK_SUCCESS && (f = fopen(tmp, "wb"))) {
        int ok = fwrite(data, 1, size, f) == size;
        ok &= fclose(f) == 0;
        if (!ok || rename(tmp, e->cache_path)) {
            unlink(tmp);
            logf_(e, PI_LOG_WARNING, "pipeline cache %s not written", e->cache_path);
        }
    }
    free(data);
}

/* ---- create and destroy --------------------------------------------------------- */

static int has_ext(VkPhysicalDevice p, const char *name)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, NULL, &n, NULL);
    VkExtensionProperties *x = calloc(n ? n : 1, sizeof(*x));
    if (!x)
        return 0;
    vkEnumerateDeviceExtensionProperties(p, NULL, &n, x);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++)
        found = !strcmp(x[i].extensionName, name);
    free(x);
    return found;
}

/* The output rendered in place needs R32G32_UINT colour attachments on the
 * linear DRM format modifier, in both of the device's lists of modifier
 * features (lavapipe's second has none: it then copies). */
static int linear_attachments(VkPhysicalDevice phys, uint32_t api)
{
    VkDrmFormatModifierPropertiesEXT mods[32];
    VkDrmFormatModifierProperties2EXT mods2[32];
    VkDrmFormatModifierPropertiesListEXT list = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkDrmFormatModifierPropertiesList2EXT list2 = {.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
    int two = api >= VK_API_VERSION_1_3;
    if (two)
        list.pNext = &list2;
    VkFormatProperties2 fp = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list};
    vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_R32G32_UINT, &fp);
    list.drmFormatModifierCount = list.drmFormatModifierCount > 32 ? 32 : list.drmFormatModifierCount;
    list.pDrmFormatModifierProperties = mods;
    list2.drmFormatModifierCount = list2.drmFormatModifierCount > 32 ? 32 : list2.drmFormatModifierCount;
    list2.pDrmFormatModifierProperties = mods2;
    vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_R32G32_UINT, &fp);
    int ok = 0, ok2 = !two;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++)
        ok |= mods[i].drmFormatModifier == 0 /* DRM_FORMAT_MOD_LINEAR */ &&
              (mods[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    for (uint32_t i = 0; two && i < list2.drmFormatModifierCount; i++)
        ok2 |= mods2[i].drmFormatModifier == 0 &&
               (mods2[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT);
    return ok && ok2;
}

static int pick_device(struct pi_engine *e, VkPhysicalDeviceProperties *props)
{
    uint32_t n = 0;
    VKC(e, vkEnumeratePhysicalDevices(e->inst, &n, NULL));
    VkPhysicalDevice *devs = calloc(n ? n : 1, sizeof(*devs));
    if (!devs)
        return PI_E_NOMEM;
    VkResult r = vkEnumeratePhysicalDevices(e->inst, &n, devs);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        free(devs);
        return PI_E_VULKAN;
    }
    const char *want = e->o.device;
    for (int pass = 0; pass < 2 && !e->phys; pass++)
        for (uint32_t i = 0; i < n && !e->phys; i++) {
            vkGetPhysicalDeviceProperties(devs[i], props);
            if (want ? strstr(props->deviceName, want) != NULL
                     : pass == 1 || props->deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
                e->phys = devs[i];
        }
    free(devs);
    if (!e->phys) {
        logf_(e, PI_LOG_ERROR, "no Vulkan device%s%s", want ? " matching " : "", want ? want : "");
        return PI_E_VULKAN;
    }
    vkGetPhysicalDeviceProperties(e->phys, props);
    return 0;
}

static int create_device(struct pi_engine *e)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pi-dv",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VKC(e, vkCreateInstance(&ici, NULL, &e->inst));
    VkPhysicalDeviceProperties props;
    int rc = pick_device(e, &props);
    if (rc)
        return rc;
    vkGetPhysicalDeviceMemoryProperties(e->phys, &e->mp);
    e->ts_period = props.limits.timestampPeriod;
    const char *exts[8] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    uint32_t next = 2;
    for (uint32_t i = 0; i < next; i++)
        if (!has_ext(e->phys, exts[i])) {
            logf_(e, PI_LOG_ERROR, "%s lacks %s", props.deviceName, exts[i]);
            return PI_E_VULKAN;
        }
    if (has_ext(e->phys, "VK_EXT_image_drm_format_modifier") && !(e->o.measure & PI_MEASURE_COPY_OUTPUT) &&
        linear_attachments(e->phys, props.apiVersion)) {
        exts[next++] = "VK_EXT_image_drm_format_modifier";
        e->modifiers = 1;
    }
    if (has_ext(e->phys, "VK_EXT_queue_family_foreign")) {
        exts[next++] = "VK_EXT_queue_family_foreign";
        e->foreign = 1;
    }
    if (has_ext(e->phys, "VK_KHR_external_semaphore_fd")) {
        exts[next++] = "VK_KHR_external_semaphore_fd";
        e->sem_fd = 1;
    }
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(e->phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq ? nq : 1, sizeof(*qf));
    if (!qf)
        return PI_E_NOMEM;
    vkGetPhysicalDeviceQueueFamilyProperties(e->phys, &nq, qf);
    e->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && e->qfam == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            e->qfam = i;
    if (e->qfam == UINT32_MAX) {
        free(qf);
        logf_(e, PI_LOG_ERROR, "%s has no graphics queue", props.deviceName);
        return PI_E_VULKAN;
    }
    e->profile = e->o.profile && qf[e->qfam].timestampValidBits;
    free(qf);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = e->qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    VKC(e, vkCreateDevice(e->phys, &dci, NULL, &e->dev));
    vkGetDeviceQueue(e->dev, e->qfam, 0, &e->queue);
    e->get_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(e->dev, "vkGetMemoryFdPropertiesKHR");
    if (e->sem_fd) {
        e->import_sem_fd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(e->dev, "vkImportSemaphoreFdKHR");
        e->get_sem_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(e->dev, "vkGetSemaphoreFdKHR");
        e->sem_fd = e->import_sem_fd && e->get_sem_fd;
    }
    if (!e->get_fd_props)
        return PI_E_VULKAN;
    snprintf(e->name, sizeof(e->name), "%s, MMR %s, output %s", props.deviceName, mode_name(e->o.mmr),
             e->modifiers ? "rendered in place" : "copied");
    return 0;
}

static int create_ctx(struct pi_engine *e, struct ctx *c)
{
    void *map;
    int rc;
    if ((rc = host_buffer(e, T_WORDS * 4, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, &c->tables, &c->tables_mem, &map)))
        return rc;
    c->tmap = map;
    if ((rc = host_buffer(e, sizeof(((dvvk_plan *)0)->reshape), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &c->reshape,
                          &c->reshape_mem, &c->rmap)))
        return rc;
    if ((rc = host_buffer(e, sizeof(struct params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &c->ubo, &c->ubo_mem, &map)))
        return rc;
    c->pmap = map;
    VkBufferViewCreateInfo bvi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = c->tables,
                                  .format = VK_FORMAT_R32_UINT, .range = VK_WHOLE_SIZE};
    VKC(e, vkCreateBufferView(e->dev, &bvi, NULL, &c->tview));
    bvi.format = VK_FORMAT_R32G32_UINT;
    VKC(e, vkCreateBufferView(e->dev, &bvi, NULL, &c->tview64));
    bvi.format = VK_FORMAT_R32G32B32A32_UINT;
    VKC(e, vkCreateBufferView(e->dev, &bvi, NULL, &c->tview128));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = e->dpool, .descriptorSetCount = 1, .pSetLayouts = &e->dsl};
    VKC(e, vkAllocateDescriptorSets(e->dev, &dai, &c->ds));
    VkDescriptorBufferInfo ub = {c->ubo, 0, VK_WHOLE_SIZE}, rb = {c->reshape, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[5] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &ub},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &c->tview},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &c->tview64},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = 10, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .pTexelBufferView = &c->tview128},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds, .dstBinding = 11, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &rb}};
    vkUpdateDescriptorSets(e->dev, 5, wr, 0, NULL);
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = e->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VKC(e, vkAllocateCommandBuffers(e->dev, &cai, &c->cmd));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKC(e, vkCreateFence(e->dev, &fci, NULL, &c->fence));
    if (e->sem_fd) {
        VkExportSemaphoreCreateInfo ex = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                          .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(e, vkCreateSemaphore(e->dev, &sci, NULL, &c->acquire));
        sci.pNext = &ex;
        VKC(e, vkCreateSemaphore(e->dev, &sci, NULL, &c->release));
    }
    if (e->profile) {
        VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 3};
        VKC(e, vkCreateQueryPool(e->dev, &qpi, NULL, &c->qp));
    }
    return 0;
}

static int create(struct pi_engine *e)
{
    int rc = create_device(e);
    if (rc)
        return rc;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = e->qfam};
    VKC(e, vkCreateCommandPool(e->dev, &pci, NULL, &e->pool));
    VkSamplerCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VKC(e, vkCreateSampler(e->dev, &smi, NULL, &e->sampler));
    VkDescriptorSetLayoutBinding b[NB];
    VkDescriptorPoolSize sizes[NB];
    for (int i = 0; i < NB; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){bindings[i].binding, bindings[i].type, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                              NULL};
        sizes[i] = (VkDescriptorPoolSize){bindings[i].type, PI_FRAMES_IN_FLIGHT};
    }
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = NB, .pBindings = b};
    VKC(e, vkCreateDescriptorSetLayout(e->dev, &dli, NULL, &e->dsl));
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &e->dsl};
    VKC(e, vkCreatePipelineLayout(e->dev, &pli, NULL, &e->pl));
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = PI_FRAMES_IN_FLIGHT, .poolSizeCount = NB, .pPoolSizes = sizes};
    VKC(e, vkCreateDescriptorPool(e->dev, &dpi, NULL, &e->dpool));
    const VkFormat f1[2] = {VK_FORMAT_R16G16_UINT, VK_FORMAT_R16_UINT},
                   f2[2] = {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT};
    if ((rc = render_pass(e, 2, f1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, &e->rp[0])) ||
        (rc = render_pass(e, 2, f2, e->modifiers ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1,
                          &e->rp[1])))
        return rc;
    cache_load(e);
    for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++)
        if ((rc = create_ctx(e, &e->c[k])))
            return rc;
    if (!pthread_create(&e->worker, NULL, worker_main, e))
        e->worker_started = 1;
    return 0;
}

void pi_options_default(struct pi_options *o)
{
    memset(o, 0, sizeof(*o));
    o->mmr = PI_MMR_EXACT;
    o->filters = PI_FILTERS_CB1;
    o->log_level = PI_LOG_WARNING;
}

int pi_engine_create(const struct pi_options *o, struct pi_engine **out)
{
    *out = NULL;
    if (!o || (o->mmr != PI_MMR_FLOAT && o->mmr != PI_MMR_EXACT64 && o->mmr != PI_MMR_SPLIT && o->mmr != PI_MMR_EXACT) ||
        (o->filters != PI_FILTERS_CB1 && o->filters != PI_FILTERS_PHASE))
        return PI_E_INVALID;
    struct pi_engine *e = calloc(1, sizeof(*e));
    if (!e)
        return PI_E_NOMEM;
    e->o = *o;   /* o->device is read during create only */
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->wake, NULL);
    e->cache_path = o->pipeline_cache ? strdup(o->pipeline_cache) : NULL;
    e->plan = calloc(1, sizeof(dvvk_plan));
    e->tables = calloc(T_WORDS, 4);
    e->state = dvvk_plan_state_new();
    int rc = !e->plan || !e->tables || !e->state || (o->pipeline_cache && !e->cache_path) ? PI_E_NOMEM : create(e);
    e->o.device = NULL;   /* the caller's strings aren't kept */
    e->o.pipeline_cache = NULL;
    if (rc) {
        pi_engine_destroy(e);
        return rc;
    }
    *out = e;
    return 0;
}

const char *pi_engine_describe(const struct pi_engine *e) { return e->name; }

/* ---- renders in flight ----------------------------------------------------------- */

/* Context c's render is done: its timings, frames and imports released. */
static int ctx_retire(struct pi_engine *e, struct ctx *c, int block)
{
    if (!c->busy)
        return 0;
    VkResult r = block ? vkWaitForFences(e->dev, 1, &c->fence, VK_TRUE, UINT64_MAX) : vkGetFenceStatus(e->dev, c->fence);
    if (r == VK_NOT_READY || r == VK_TIMEOUT)
        return 1;
    if (r != VK_SUCCESS) {
        logf_(e, PI_LOG_ERROR, "render failed: %d", (int)r);
        /* the device is lost: nothing will run; release anyway */
    }
    vkResetFences(e->dev, 1, &c->fence);
    c->busy = 0;
    if (c->qp) {
        uint64_t ts[3];
        if (vkGetQueryPoolResults(e->dev, c->qp, 0, 3, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (int i = 0; i < 2; i++)
                c->stats.pass_ms[i] = (double)(ts[i + 1] - ts[i]) * e->ts_period / 1e6;
    }
    if (c->out && c->out->serial == c->serial)
        c->out->stats = c->stats;
    c->out = NULL;
    av_frame_free(&c->bl);
    av_frame_free(&c->el);
    for (int i = 0; i < 4; i++)
        if (c->in[i]) {
            c->in[i]->refs--;
            c->in[i] = NULL;
        }
    return 0;
}

static void retire_all(struct pi_engine *e)
{
    for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++)
        ctx_retire(e, &e->c[k], 1);
}

int pi_output_done(struct pi_engine *e, struct pi_output *o)
{
    if (!o->serial)
        return 1;
    struct ctx *c = &e->c[o->ctx];
    return c->serial != o->serial || ctx_retire(e, c, 0) == 0;
}

int pi_output_wait(struct pi_engine *e, struct pi_output *o)
{
    if (o->serial) {
        struct ctx *c = &e->c[o->ctx];
        if (c->serial == o->serial)
            ctx_retire(e, c, 1);
    }
    return 0;
}

void pi_output_stats(struct pi_engine *e, struct pi_output *o, struct pi_render_stats *s)
{
    pi_output_done(e, o);
    *s = o->stats;
}

/* ---- streams ---------------------------------------------------------------- */

static void stream_images_destroy(struct pi_engine *e)
{
    for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++) {
        struct ctx *c = &e->c[k];
        vkDestroyFramebuffer(e->dev, c->fb, NULL);
        c->fb = VK_NULL_HANDLE;
        image_destroy(e, &c->c420);
        image_destroy(e, &c->ely);
    }
}

void pi_stream_close(struct pi_engine *e)
{
    if (!e || !e->dev)
        return;
    retire_all(e);
    imports_release(e);
    e->open = 0;
}

int pi_stream_open(struct pi_engine *e, const struct pi_stream_info *info)
{
    if (!info || info->width < 16 || info->height < 16 || (info->width | info->height) & 1 || info->width > 8192 ||
        info->height > 4320)
        return PI_E_INVALID;
    pi_stream_close(e);
    dvvk_plan_state_reset(e->state);
    e->have_tables = 0;
    for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++)
        e->c[k].tables_serial = 0;
    e->first = 1;
    e->profile_dv = info->profile;
    if (info->width != e->w || info->height != e->h || !e->c[0].fb) {
        stream_images_destroy(e);
        e->w = info->width;
        e->h = info->height;
        const uint32_t cw = e->w / 2, ch = e->h / 2;
        const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++) {
            struct ctx *c = &e->c[k];
            int rc;
            if ((rc = image(e, &c->c420, VK_FORMAT_R16G16_UINT, cw, ch, rt)) ||
                (rc = image(e, &c->ely, VK_FORMAT_R16_UINT, cw, ch, rt))) {
                stream_images_destroy(e);
                e->w = e->h = 0;
                return rc;
            }
            VkImageView v[2] = {c->c420.view, c->ely.view};
            VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = e->rp[0],
                                           .attachmentCount = 2, .pAttachments = v, .width = cw, .height = ch,
                                           .layers = 1};
            VKC(e, vkCreateFramebuffer(e->dev, &fbi, NULL, &c->fb));
            VkDescriptorImageInfo ii[2] = {{e->sampler, c->c420.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                           {e->sampler, c->ely.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet wr[2];
            for (int i = 0; i < 2; i++)
                wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds,
                                               .dstBinding = 7u + (uint32_t)i, .descriptorCount = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                               .pImageInfo = &ii[i]};
            vkUpdateDescriptorSets(e->dev, 2, wr, 0, NULL);
        }
    }
    e->open = 1;
    /* P2's bars pipeline is the same for every stream */
    uint32_t kb[K2_N] = {[K2_BARS] = 1};
    return build(e, 1, kb) ? 0 : PI_E_VULKAN;
}

/* ---- outputs ----------------------------------------------------------------- */

int pi_output_size(uint32_t width, uint32_t height, uint32_t *stride, uint64_t *bytes)
{
    if (width < 2 || height < 2 || (width | height) & 1 || width > 8192 || height > 4320)
        return PI_E_INVALID;
    *stride = (width * 4 + 255) & ~255u;
    *bytes = (((uint64_t)*stride * (height + 1) + 4095) & ~(uint64_t)4095) + 4096;
    return 0;
}

static void output_destroy(struct pi_engine *e, struct pi_output *o)
{
    vkDestroyFramebuffer(e->dev, o->fb, NULL);
    for (int i = 0; i < 2; i++)
        image_destroy(e, &o->half[i]);
    vkDestroyBuffer(e->dev, o->copy_buf, NULL);
    vkFreeMemory(e->dev, o->copy_mem, NULL);
    free(o);
}

/* Output row parity `half` (0 even rows, 1 odd rows) of the dmabuf: a linear
 * RG32UI image of w/2 x h/2 pixel pairs, rows 2 * stride apart, from byte
 * half * stride. P2 renders both as its two colour attachments: one
 * fragment, a 2 x 2 block of pixels. (Four images and 2 x 4 blocks, fewer
 * fetches per pixel, measured slower: 32 bytes a pixel of tile buffer
 * shrinks the tiles.) */
static int output_half(struct pi_engine *e, struct pi_output *o, int fd, int half)
{
    struct image *im = &o->half[half];
    if (!e->modifiers)
        return image(e, im, VK_FORMAT_R32G32_UINT, o->w / 2, o->h / 2,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkSubresourceLayout plane = {.offset = (VkDeviceSize)half * o->stride, .rowPitch = 2 * (VkDeviceSize)o->stride};
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .pNext = &ext,
        .drmFormatModifier = 0 /* DRM_FORMAT_MOD_LINEAR */, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &mod, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R32G32_UINT, .extent = {o->w / 2, o->h / 2, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(e, vkCreateImage(e->dev, &ci, NULL, &im->img));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(e->dev, im->img, &r);
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = im->img};
    int rc = import_memory(e, fd, &r, &ded, &im->mem);
    if (rc)
        return rc;
    VKC(e, vkBindImageMemory(e->dev, im->img, im->mem, 0));
    return view(e, im, VK_FORMAT_R32G32_UINT);
}

static int output_build(struct pi_engine *e, struct pi_output *o, int fd)
{
    int rc;
    for (int half = 0; half < 2; half++)
        if ((rc = output_half(e, o, fd, half)))
            return rc;
    if (!e->modifiers) {
        /* copy mode: our own images, copied into the dmabuf */
        VkExternalMemoryBufferCreateInfo bext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                 .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &bext,
                                  .size = (VkDeviceSize)o->stride * o->h, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKC(e, vkCreateBuffer(e->dev, &bci, NULL, &o->copy_buf));
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(e->dev, o->copy_buf, &r);
        if ((rc = import_memory(e, fd, &r, NULL, &o->copy_mem)))
            return rc;
        VKC(e, vkBindBufferMemory(e->dev, o->copy_buf, o->copy_mem, 0));
    }
    VkImageView views[2] = {o->half[0].view, o->half[1].view};
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = e->rp[1],
                                   .attachmentCount = 2, .pAttachments = views, .width = o->w / 2,
                                   .height = o->h / 2, .layers = 1};
    VKC(e, vkCreateFramebuffer(e->dev, &fbi, NULL, &o->fb));
    return 0;
}

int pi_output_import(struct pi_engine *e, int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t size,
                     struct pi_output **out)
{
    *out = NULL;
    uint32_t min_stride;
    uint64_t bytes;
    if (fd < 0 || pi_output_size(width, height, &min_stride, &bytes) || stride < width * 4 || stride % 4 ||
        size < (uint64_t)stride * (height + 1)) {
        logf_(e, PI_LOG_ERROR, "output %ux%u, stride %u, %llu bytes: needs a stride of %u or more and %llu bytes",
              width, height, stride, (unsigned long long)size, width * 4, (unsigned long long)stride * (height + 1));
        return PI_E_INVALID;
    }
    off_t real = dmabuf_size(fd);
    if (real > 0 && (uint64_t)real < size) {
        logf_(e, PI_LOG_ERROR, "output dmabuf has %lld bytes, not %llu", (long long)real, (unsigned long long)size);
        return PI_E_INVALID;
    }
    struct pi_output *o = calloc(1, sizeof(*o));
    if (!o)
        return PI_E_NOMEM;
    o->w = width;
    o->h = height;
    o->stride = stride;
    int rc = output_build(e, o, fd);
    if (rc) {
        output_destroy(e, o);
        return rc;
    }
    o->next = e->outputs;
    e->outputs = o;
    *out = o;
    return 0;
}

void pi_output_release(struct pi_engine *e, struct pi_output *o)
{
    if (!o)
        return;
    pi_output_wait(e, o);
    for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++)
        if (e->c[k].out == o)
            ctx_retire(e, &e->c[k], 1);
    for (struct pi_output **p = &e->outputs; *p; p = &(*p)->next)
        if (*p == o) {
            *p = o->next;
            break;
        }
    output_destroy(e, o);
}

void pi_engine_destroy(struct pi_engine *e)
{
    if (!e)
        return;
    worker_stop(e);
    if (e->dev) {
        vkDeviceWaitIdle(e->dev);
        retire_all(e);
        imports_release(e);
        while (e->outputs) {
            struct pi_output *o = e->outputs;
            e->outputs = o->next;
            output_destroy(e, o);
        }
        stream_images_destroy(e);
        for (int k = 0; k < PI_FRAMES_IN_FLIGHT; k++) {
            struct ctx *c = &e->c[k];
            vkDestroyQueryPool(e->dev, c->qp, NULL);
            vkDestroySemaphore(e->dev, c->acquire, NULL);
            vkDestroySemaphore(e->dev, c->release, NULL);
            vkDestroyFence(e->dev, c->fence, NULL);
            vkDestroyBufferView(e->dev, c->tview, NULL);
            vkDestroyBufferView(e->dev, c->tview64, NULL);
            vkDestroyBufferView(e->dev, c->tview128, NULL);
            vkDestroyBuffer(e->dev, c->tables, NULL);
            vkDestroyBuffer(e->dev, c->ubo, NULL);
            vkDestroyBuffer(e->dev, c->reshape, NULL);
            vkFreeMemory(e->dev, c->tables_mem, NULL);   /* unmaps */
            vkFreeMemory(e->dev, c->ubo_mem, NULL);
            vkFreeMemory(e->dev, c->reshape_mem, NULL);
        }
        for (int s = 0; s < 2; s++) {
            struct variants *v = set_of(e, s);
            for (int i = 0; i < v->n; i++)
                vkDestroyPipeline(e->dev, v->v[i].p, NULL);
        }
        cache_save(e);
        vkDestroyPipelineCache(e->dev, e->cache, NULL);
        vkDestroyRenderPass(e->dev, e->rp[0], NULL);
        vkDestroyRenderPass(e->dev, e->rp[1], NULL);
        vkDestroyDescriptorPool(e->dev, e->dpool, NULL);   /* frees the sets */
        vkDestroyPipelineLayout(e->dev, e->pl, NULL);
        vkDestroyDescriptorSetLayout(e->dev, e->dsl, NULL);
        vkDestroySampler(e->dev, e->sampler, NULL);
        vkDestroyCommandPool(e->dev, e->pool, NULL);   /* frees the command buffers */
        vkDestroyDevice(e->dev, NULL);
    }
    if (e->inst)
        vkDestroyInstance(e->inst, NULL);
    free(e->v1.v);
    free(e->v2.v);
    if (e->state)
        dvvk_plan_state_free(e->state);
    free(e->plan);
    free(e->tables);
    free(e->cache_path);
    pthread_cond_destroy(&e->wake);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

/* ---- the tables of a planner generation --------------------------------------- */

/* n little-endian 32-bit words, two's complement, as a double. */
static double words_value(const uint32_t *w, int n)
{
    int negative = (w[n - 1] & 0x80000000u) != 0;
    uint32_t v[4];
    uint32_t carry = 1;
    for (int i = 0; i < n; i++) {   /* |value| */
        v[i] = negative ? ~w[i] + carry : w[i];
        carry = negative && carry && v[i] == 0;
    }
    double d = 0;
    for (int i = n - 1; i >= 0; i--)
        d = d * 4294967296.0 + v[i];
    return negative ? -d : d;
}

/* (a * b) >> 20, as the shaders' feature product. */
static uint64_t fprod(uint64_t a, uint64_t b) { return ((a * b) >> 20) & 0xffffffffu; }

/* The float MMR (modes 0, 4) of channel c: constants, cross coefficients and
 * the univariate LUTs, all / 2^(shift + 4), for any MMR kind. */
static void float_mmr(const dvvk_plan *p, unsigned c, struct params *q, float *lutf)
{
    const uint32_t *w = p->mmr_params[c];
    unsigned kind = p->chroma_kind[c], order = w[0];
    double scale = ldexp(1.0, -(int)(w[1] + 4));
    /* coefficient (feature k, order o): kind 3 int32, 2 int64, 1 int128 */
    int n = kind == DVVK_CHROMA_MMR64_LUT ? 1 : kind == DVVK_CHROMA_MMR96 ? 2 : 4;
    double constant = words_value(w + 8, kind == DVVK_CHROMA_MMR64_LUT ? 2 : kind == DVVK_CHROMA_MMR96 ? 3 : 4);
    q->cconst[c] = (float)(constant * scale);
#define COEF(k, o) words_value(w + 12 + 28 * (o) + 4 * (k), n)
    for (unsigned o = 0; o < 3; o++)
        for (unsigned k = 0; k < 4; k++)
            q->cf[c * 3 + o][k] = o < order ? (float)(COEF(k + 3, o) * scale) : 0.0f;
    for (unsigned t = 0; t < 3; t++)
        for (unsigned i = 0; i < 1024; i++) {
            double v;
            if (kind == DVVK_CHROMA_MMR64_LUT)
                v = (double)p->mmr_lut[c][t][i] * scale;
            else {
                uint64_t f1 = (uint64_t)i << 10, f2 = fprod(f1, f1), f3 = fprod(f1, f2);
                v = COEF(t, 0) * (double)f1;
                if (order > 1)
                    v += COEF(t, 1) * (double)f2;
                if (order > 2)
                    v += COEF(t, 2) * (double)f3;
                v *= scale;
            }
            lutf[(t * 1024 + i) * 2 + c] = (float)v;   /* Cb, Cr interleaved */
        }
#undef COEF
}

/* The 128-bit MMR parameters of channel c (p1_chroma.frag mmr_wide): the
 * constant, then coefficient (o, k) at 1 + 7o + k, four words each. */
static void wide_tables(const dvvk_plan *p, unsigned c, uint32_t *t)
{
    const uint32_t *w = p->mmr_params[c];
    uint32_t *d = t + T_WIDE + c * WIDE_WORDS;
    memcpy(d, w + 8, 4 * 4);
    for (unsigned i = 0; i < 21; i++)
        memcpy(d + 4 * (1 + i), w + 12 + 4 * i, 4 * 4);
}

/* Route 3 with one piece per chroma channel, its sums within 64 bits (the
 * planner's narrow mask), 10-bit: the same sum as the FEL MMR's, so pass 1's
 * MMR computes it (exactly in every exact mode). Fills the MMR parameters
 * and the univariate tables (exact 64-bit, and float) from the pieces; 0 if
 * this mapping is outside that. *fits32: every cross coefficient fits 32
 * bits (emulated 64-bit, mode 2, multiplies 32-bit coefficients). */
static int piece_params(const dvvk_plan *p, struct params *q, uint32_t *t, uint32_t order[2], int *fits32)
{
    struct reshape r;
    memcpy(&r, p->reshape, sizeof(r));
    if (r.base_depth != 10)
        return 0;
    for (int c = 0; c < 3; c++)
        if (r.curve[c].pivot_count < 2 || r.curve[c].pivot_count > 9)
            return 0;
    for (int c = 1; c < 3; c++) {
        const struct reshape_curve *cv = &r.curve[c];
        const struct reshape_piece *pc = &cv->piece[0];
        if (cv->pivot_count != 2 || !(p->narrow_mask[c] & 1u) || pc->order < 1 || pc->order > 3 ||
            (pc->method == 0 && pc->order > 2) || (p->order2 && !(pc->method != 0 && pc->order == 2)))
            return 0;
    }
    for (int k = 0; k < 3; k++) {   /* the input clamps: each component by its own curve */
        q->lo[0][k] = r.curve[k].pivots[0];
        q->hi[0][k] = r.curve[k].pivots[r.curve[k].pivot_count - 1];
    }
    float *lutf = (float *)(t + T_LUTF);
    double scale = ldexp(1.0, -(int)(r.denominator + 4));   /* the float MMR's: Q16 */
    *fits32 = 1;
    for (int c = 1; c < 3; c++) {
        const struct reshape_piece *pc = &r.curve[c].piece[0];
        int ch = c - 1, mmr = pc->method != 0;
        order[ch] = mmr ? pc->order : 1;
        int64_t constant = (int64_t)((uint64_t)pc->constant << 20);
        q->const64[ch][0] = (uint32_t)(uint64_t)constant;
        q->const64[ch][1] = (uint32_t)((uint64_t)constant >> 32);
        q->cy[ch] = (float)((double)constant / 4294967296.0);
        q->cconst[ch] = (float)((double)constant * scale);
        q->pk[1 + ch] = r.denominator;   /* the shader adds 4: denominator + q - 16 */
        for (int o = 0; o < 3; o++)
            for (int k = 0; k < 4; k++) {
                int64_t v = mmr && o < (int)pc->order ? pc->coefficient[o][k + 3] : 0;
                q->c32[ch * 3 + o][k] = (int32_t)(uint32_t)(uint64_t)v;   /* the low word: it wraps */
                q->cs[ch * 3 + o][k] = (float)((double)v / 4294967296.0);
                q->cf[ch * 3 + o][k] = (float)((double)v * scale);
                if (v != (int32_t)v)
                    *fits32 = 0;
            }
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 1024; i++) {
                __int128 v = 0;
                int64_t f1 = (int64_t)i << 10, f2 = (int64_t)i * i, f3 = (f1 * f2) >> 20;
                if (mmr) {
                    v += (__int128)pc->coefficient[0][k] * f1;
                    if (pc->order > 1)
                        v += (__int128)pc->coefficient[1][k] * f2;
                    if (pc->order > 2)
                        v += (__int128)pc->coefficient[2][k] * f3;
                } else if (k == c) {   /* a polynomial of the channel's own value */
                    v += (__int128)pc->coefficient[0][0] * f1;
                    if (pc->order == 2)
                        v += (__int128)pc->coefficient[0][1] * f2;
                }
                if (v > INT64_MAX || v < INT64_MIN)
                    return 0;
                uint64_t u = (uint64_t)(int64_t)v;
                t[T_LUT64 + (k * 1024 + i) * 4 + ch * 2] = (uint32_t)u;
                t[T_LUT64 + (k * 1024 + i) * 4 + ch * 2 + 1] = (uint32_t)(u >> 32);
                lutf[(k * 1024 + i) * 2 + ch] = (float)((double)(int64_t)v * scale);
            }
    }
    return 1;
}

/* Mode 3 (p1_chroma.frag, the split): the high word from a float sum y of
 * S / 2^32 is exact when y is within 0.5 of it (round()). Every float
 * operation rounds within u = 2^-24 of its result (Vulkan: add and multiply
 * correctly rounded). Per channel, m the sum of the 16 terms' largest
 * magnitudes (constant, 3 tables, 12 cross terms): each term's own roundings
 * (its coefficient's or entry's conversion, the product or the entry's inner
 * add) are within 2u of it, together 2um; its add into y within um, any order
 * (every partial sum is within m); the final subtraction um. About 19um:
 * 32um < 0.5 keeps 1.7x room. Returns 0 where the bound fails for channel
 * c's largest magnitudes: the frame then checks each sample (the guard). */
static int split_ok(const struct pi_engine *e, const struct params *q, const uint32_t *t, unsigned c)
{
    double m = fabs((double)(int64_t)((uint64_t)q->const64[c][1] << 32 | q->const64[c][0]));
    for (int k = 0; k < 3; k++) {
        double lm = 0;
        for (int i = 0; i < 1024; i++) {
            const uint32_t *w = t + T_LUT64 + (k * 1024 + i) * 4 + c * 2;
            lm = fmax(lm, fabs((double)(int64_t)((uint64_t)w[1] << 32 | w[0])));
        }
        m += lm;
    }
    for (int o = 0; o < 3; o++)
        for (int k = 0; k < 4; k++)
            m += fabs((double)q->cs[c * 3 + o][k]) * 4294967296.0 * 1048576.0;   /* features < 2^20 */
    m /= 4294967296.0;
    double bound = 32.0 / 16777216.0 * m;
    logf_(e, PI_LOG_DEBUG, "MMR channel %u split: largest |S| / 2^32 %.1f, float bound %.3g (< 0.5)", c, m, bound);
    return bound < 0.5 && m < 1048576.0;
}

/* The plan's route, 0 if these passes can't render it. */
static int route_of(const struct pi_engine *e, const dvvk_plan *p, int has_el, struct route *rt)
{
    memset(rt, 0, sizeof(*rt));
    rt->single = p->route == DVVK_ROUTE_SINGLE;
    if (p->route == DVVK_ROUTE_PIECEWISE && p->depth == 10 && p->layout <= 1 &&
        p->pack == (p->layout ? DVVK_PACK_422 : DVVK_PACK_SITED))
        rt->piece = 1 + (int)p->layout;
    if ((!rt->single && !rt->piece && (p->route != DVVK_ROUTE_DUAL || !has_el || p->equal || p->nearest != 2)) ||
        (!rt->piece && p->pack != DVVK_PACK_SITED) || p->placement[0] || p->placement[1] ||
        p->placement[2] != e->w || p->placement[3] != e->h)
        return 0;
    return 1;
}

static void route_signature(const dvvk_plan *p, int has_el, uint32_t sig[16])
{
    uint32_t s[16] = {p->route, p->depth, p->layout, p->pack, p->order2, p->sampling, p->chroma_kind[0],
                      p->chroma_kind[1], p->dual, p->narrow_mask[0], p->narrow_mask[1], p->narrow_mask[2],
                      p->narrow_mask[3], (uint32_t)has_el, p->equal, p->nearest};
    memcpy(sig, s, sizeof(s));
}

/* The tables, the table-derived Params and the route of the planner's current
 * generation; rebuilt only when it changes. */
static void prepare(struct pi_engine *e, const dvvk_plan *p, int has_el)
{
    uint32_t sig[16];
    route_signature(p, has_el, sig);
    if (e->have_tables && p->tables_generation == e->tables_gen && !memcmp(sig, e->sig, sizeof(sig)))
        return;
    struct route *rt = &e->route;
    route_of(e, p, has_el, rt);
    struct params *q = &e->tmpl;
    uint32_t *t = e->tables;
    memset(q, 0, sizeof(*q));
    memset(t, 0, T_WORDS * 4);
    for (int i = 0; i < 1024; i++) {
        t[T_MAP + i] = p->mapping[i] + 8u;   /* the pack's rounding, folded in */
        t[T_CMAP + i] = p->chroma_mapping[0][i];
        t[T_CMAP + 1024 + i] = p->chroma_mapping[1][i];
    }
    memcpy(t + T_RES, p->residual, sizeof(p->residual));
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 1024; i++)
            t[T_SINGLE + c * 1024 + i] = rt->piece ? (c == 0 ? p->luma_table[i] : 0u) : p->single_table[c][i];
    if (rt->piece) {
        memcpy(q->narrow, p->narrow_mask, sizeof(q->narrow));
        q->pw[0] = p->sampling;
    }
    q->err[0] = 20.0f / 16777216.0f;   /* mode 4: p1_chroma.frag mmr_float_bounded */
    q->err[1] = 32.0f / 16777216.0f;   /* the split's guard: split_ok's bound per sample */
    q->pk[1] = p->mmr_params[0][1];
    q->pk[2] = p->mmr_params[1][1];
    rt->mode = e->o.mmr;
    int split = rt->mode == PI_MMR_SPLIT || rt->mode == PI_MMR_EXACT, force_wide = e->o.measure & PI_MEASURE_FORCE_WIDE;
    for (unsigned c = 0; c < 2; c++) {
        const uint32_t *pw = p->mmr_params[c];
        for (int k = 0; k < 3; k++) {
            q->lo[c][k] = pw[2 + 2 * k];
            q->hi[c][k] = pw[3 + 2 * k];
        }
        unsigned kind = p->chroma_kind[c];
        if (kind == DVVK_CHROMA_POLY)
            continue;
        if (kind != DVVK_CHROMA_MMR64_LUT || force_wide) {
            rt->wide = 1;
            wide_tables(p, c, t);
        }
        q->const64[c][0] = pw[8];
        q->const64[c][1] = pw[9];
        for (unsigned o = 0; o < 3; o++)
            for (unsigned k = 0; k < 4; k++)
                q->c32[c * 3 + o][k] = o < pw[0] ? (int32_t)pw[12 + 28 * o + 4 * (k + 3)] : 0;
        float_mmr(p, c, q, (float *)(t + T_LUTF));
        if (kind != DVVK_CHROMA_MMR64_LUT)
            continue;
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 1024; i++) {
                uint64_t v = (uint64_t)p->mmr_lut[c][k][i];
                t[T_LUT64 + (k * 1024 + i) * 4 + c * 2] = (uint32_t)v;   /* Cb lo, hi, Cr lo, hi */
                t[T_LUT64 + (k * 1024 + i) * 4 + c * 2 + 1] = (uint32_t)(v >> 32);
            }
        q->cy[c] = (float)((double)(int64_t)((uint64_t)pw[9] << 32 | pw[8]) / 4294967296.0);
        for (int o = 0; o < 3; o++)
            for (int k = 0; k < 4; k++)
                q->cs[c * 3 + o][k] = (float)(q->c32[c * 3 + o][k] / 4294967296.0);
        if (split && !rt->wide && !split_ok(e, q, t, c))
            rt->guard = 1;
    }
    /* Route 3, one piece per chroma channel: pass 1's MMR in the stream's
     * mode. Where the split's bound fails, its guard's fallback, emulated
     * 64-bit, needs 32-bit coefficients. */
    int fits32 = 0;
    if (rt->piece && !(e->o.measure & PI_MEASURE_PIECE_GENERAL) && piece_params(p, q, t, rt->order, &fits32)) {
        int ok = rt->mode == PI_MMR_FLOAT || (split_ok(e, q, t, 0) && split_ok(e, q, t, 1));
        rt->guard = split && !ok;
        rt->fast = fits32 || (rt->mode != PI_MMR_EXACT64 && ok);
    }
    if (rt->guard)
        WARN_ONCE(e, W_GUARD, "MMR magnitudes too large for the split's frame bound: checked per sample");
    logf_(e, PI_LOG_DEBUG, "tables: generation %u, route %u%s, chroma kinds %u %u%s, dual %u, orders %u %u%s",
          p->tables_generation, p->route,
          rt->piece ? (rt->fast ? " (one MMR piece per channel)" : " (general reshape)") : "", p->chroma_kind[0],
          p->chroma_kind[1], rt->wide ? " (128-bit)" : "", p->dual, p->mmr_params[0][0], p->mmr_params[1][0],
          rt->guard ? ", split guarded" : "");
    memcpy(e->sig, sig, sizeof(sig));
    e->tables_gen = p->tables_generation;
    e->have_tables = 1;
    e->tables_serial++;
}

/* ---- per frame ------------------------------------------------------------------- */

/* The input frames' SAND128 planes as texel buffers: in[0..1] BL luma,
 * chroma; in[2..3] the EL's. */
static int bind_inputs(struct pi_engine *e, struct ctx *c, const AVFrame *bl, const AVFrame *el, struct params *q)
{
    const AVFrame *f[2] = {bl, el};
    VkBufferView views[4];
    for (int l = 0; l < 2; l++) {
        const AVDRMFrameDescriptor *d = f[l]->format == AV_PIX_FMT_DRM_PRIME ? (const void *)f[l]->data[0] : NULL;
        if (!d || d->nb_layers != 1 || d->layers[0].nb_planes != 2 || d->nb_objects < 1 ||
            d->nb_objects > AV_DRM_MAX_PLANES) {
            WARN_ONCE(e, W_INPUT, "input isn't a DRM PRIME frame with one layer of two planes");
            return PI_E_UNSUPPORTED_FORMAT;
        }
        const AVDRMLayerDescriptor *ly = &d->layers[0];
        uint64_t mod = d->objects[0].format_modifier;
        if (!pi_is_sand128(mod)) {
            WARN_ONCE(e, W_INPUT, "input isn't SAND128 (modifier %#llx)", (unsigned long long)mod);
            return PI_E_UNSUPPORTED_FORMAT;
        }
        uint32_t *o = l ? q->el : q->bl;
        if (e->first && (l == 0 || el != bl)) {
            logf_(e, PI_LOG_DEBUG, "%s %dx%d, %d objects, planes at object %d +%td (pitch %td), object %d +%td",
                  l ? "EL" : "BL", f[l]->width, f[l]->height, d->nb_objects, ly->planes[0].object_index,
                  ly->planes[0].offset, ly->planes[0].pitch, ly->planes[1].object_index, ly->planes[1].offset);
        }
        for (int p = 0; p < 2; p++) {
            int obj = ly->planes[p].object_index;
            if (obj < 0 || obj >= d->nb_objects || ly->planes[p].offset < 0 || (ly->planes[p].offset & 3))
                return PI_E_UNSUPPORTED_FORMAT;
            struct import *im = import_dmabuf(e, d->objects[obj].fd);
            if (!im) {
                WARN_ONCE(e, W_IMPORT, "decoder dmabuf import failed");
                return PI_E_VULKAN;
            }
            im->last_used = e->serial;
            im->refs++;
            c->in[l * 2 + p] = im;
            views[l * 2 + p] = im->view;
            o[p] = (uint32_t)(ly->planes[p].offset / 4);
        }
        /* Column heights: the modifier's, else (the Pi decoder: 0 there, luma
         * and chroma in separate buffers) each plane's pitch, as yblod's
         * import (interop/va.rs) and FFmpeg's SAND code take them. */
        uint32_t column = pi_sand_column_height(mod);
        o[2] = column ? column : (uint32_t)ly->planes[0].pitch;
        o[3] = column ? column : (uint32_t)ly->planes[1].pitch;
    }
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < 4; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->ds,
                                       .dstBinding = 3u + (uint32_t)i, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                       .pTexelBufferView = &views[i]};
    vkUpdateDescriptorSets(e->dev, 4, wr, 0, NULL);
    return 0;
}

/* The frame's own Params over the generation's. */
static void frame_params(struct pi_engine *e, const dvvk_plan *p, const AVFrame *el, struct params *q)
{
    uint32_t w = e->w, h = e->h;
    q->size[0] = w;
    q->size[1] = h;
    q->size[2] = w / 2;
    q->size[3] = h / 2;
    q->el_size[0] = el ? (uint32_t)el->width / 2 : w / 4;
    q->el_size[1] = el ? (uint32_t)el->height / 2 : h / 4;
    memcpy(q->active, p->active, sizeof(q->active));
    memcpy(q->black, p->black, sizeof(q->black));
    q->pk[0] = p->packet_count;
    q->pk[3] = (p->packet_count * PI_PACKET_PIXELS + w - 1) / w;   /* rows the packets reach */
    q->inv[0] = 1.0f / (float)(w / 2);
    q->inv[1] = 1.0f / (float)(h / 2);
    memcpy(q->packets, p->packets, sizeof(p->packets));
}

/* The pipelines' keys for this frame. */
static void keys(const struct pi_engine *e, const dvvk_plan *p, uint32_t k1[K1_N], uint32_t k2[K2_N])
{
    const struct route *rt = &e->route;
    memset(k1, 0, K1_N * 4);
    if (!rt->single && !rt->piece) {   /* the FEL selectors */
        k1[K1_MODE] = rt->mode;
        k1[K1_METHOD] = p->chroma_phase;
        k1[K1_DUAL] = p->dual;
        for (int c = 0; c < 2; c++) {
            uint32_t kind = p->chroma_kind[c];
            k1[K1_KIND0 + c] = kind == DVVK_CHROMA_MMR64_LUT && (e->o.measure & PI_MEASURE_FORCE_WIDE) ? DVVK_CHROMA_MMR128 : kind;
            k1[K1_ORDER0 + c] = p->mmr_params[c][0];
        }
    }
    k1[K1_SINGLE] = (uint32_t)rt->single;
    k1[K1_PIECE] = (uint32_t)rt->piece;
    k1[K1_ORDER2] = rt->piece == 2 ? p->order2 : 0u;
    k1[K1_FAST] = (uint32_t)rt->fast;
    if (rt->fast) {   /* the MMR's mode and orders */
        k1[K1_MODE] = rt->mode;
        k1[K1_ORDER0] = rt->order[0];
        k1[K1_ORDER1] = rt->order[1];
    }
    k1[K1_MARK] = (e->o.measure & PI_MEASURE_MARK) != 0;
    if (k1[K1_MODE] == PI_MMR_SPLIT || k1[K1_MODE] == PI_MMR_EXACT)
        k1[K1_GUARD] = rt->guard;
    memset(k2, 0, K2_N * 4);
    k2[K2_ANNEXB] = p->chroma_filter != 0;
    k2[K2_TOP_LEFT] = p->top_left != 0;
    k2[K2_ABLATE] = e->o.ablate;
    k2[K2_SINGLE] = (uint32_t)(rt->single || rt->piece);
    k2[K2_FULL422] = rt->piece == 2;
}

struct band {
    VkPipeline p;
    uint32_t y0, rows;
};

/* Pass i over rows y0 .. y0 + rows - 1 of its w x h target (the render area:
 * stored tiles), as bands of rows each with its pipeline. */
static void pass(struct pi_engine *e, struct ctx *c, int i, VkFramebuffer fb, uint32_t w, uint32_t h, uint32_t y0,
                 uint32_t rows, const struct band *b, int n, const VkClearValue *clear)
{
    VkClearValue cv[2];
    memset(cv, 0, sizeof(cv));
    if (clear)
        cv[0] = cv[1] = *clear;
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = e->rp[i],
                                 .framebuffer = fb, .renderArea = {{0, (int32_t)y0}, {w, rows}},
                                 .clearValueCount = clear ? 2 : 0, .pClearValues = clear ? cv : NULL};
    vkCmdBeginRenderPass(c->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->pl, 0, 1, &c->ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    vkCmdSetViewport(c->cmd, 0, 1, &vp);
    for (int k = 0; k < n; k++) {
        if (!b[k].rows)
            continue;
        vkCmdBindPipeline(c->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, b[k].p);
        VkRect2D sc = {{0, (int32_t)b[k].y0}, {w, b[k].rows}};
        vkCmdSetScissor(c->cmd, 0, 1, &sc);
        vkCmdDraw(c->cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(c->cmd);
    if (c->qp)
        vkCmdWriteTimestamp(c->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c->qp, (uint32_t)i + 1);
}

/* Queue family ownership of the external buffers (VK_EXT_queue_family_foreign):
 * the decoder's buffers and the output, acquired before the passes (to: 1)
 * and released to the display after (to: 0). */
static void ownership(struct pi_engine *e, struct ctx *c, struct pi_output *o, int acquire)
{
    if (!e->foreign)
        return;
    uint32_t ours = e->qfam, foreign = VK_QUEUE_FAMILY_FOREIGN_EXT;
    VkBufferMemoryBarrier bb[5];
    VkImageMemoryBarrier ib[2];
    uint32_t nb = 0, ni = 0;
    for (int i = 0; i < 4; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++)
            seen |= c->in[j] == c->in[i];
        if (c->in[i] && !seen)
            bb[nb++] = (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                               .dstAccessMask = acquire ? VK_ACCESS_SHADER_READ_BIT : 0,
                                               .srcQueueFamilyIndex = acquire ? foreign : ours,
                                               .dstQueueFamilyIndex = acquire ? ours : foreign,
                                               .buffer = c->in[i]->buf, .size = VK_WHOLE_SIZE};
    }
    if (o->copy_buf)
        bb[nb++] = (VkBufferMemoryBarrier){.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                           .srcAccessMask = acquire ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .dstAccessMask = acquire ? VK_ACCESS_TRANSFER_WRITE_BIT : 0,
                                           .srcQueueFamilyIndex = acquire ? foreign : ours,
                                           .dstQueueFamilyIndex = acquire ? ours : foreign, .buffer = o->copy_buf,
                                           .size = VK_WHOLE_SIZE};
    else
        for (int h = 0; h < 2; h++)
            ib[ni++] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = acquire ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = acquire ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0,
                .oldLayout = acquire ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = acquire ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = acquire ? foreign : ours, .dstQueueFamilyIndex = acquire ? ours : foreign,
                .image = o->half[h].img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkPipelineStageFlags src = acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                       : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         dst = acquire ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
                                       : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    vkCmdPipelineBarrier(c->cmd, src, dst, 0, 0, NULL, nb, bb, ni, ib);
}

static int record(struct pi_engine *e, struct ctx *c, const dvvk_plan *p, struct pi_output *o, VkPipeline p1,
                  VkPipeline p2, VkPipeline pb)
{
    uint32_t w = e->w, h = e->h, cw = w / 2, ch = h / 2;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKC(e, vkResetCommandBuffer(c->cmd, 0));
    VKC(e, vkBeginCommandBuffer(c->cmd, &bi));
    if (c->qp) {
        vkCmdResetQueryPool(c->cmd, c->qp, 0, 3);
        vkCmdWriteTimestamp(c->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, c->qp, 0);
    }
    ownership(e, c, o, 1);
    /* Pass 1 only over the letterbox's row window (yblod's, with the margin the
     * filters need; 8-aligned): pass 2 reads no chroma or EL outside it, and
     * writes black there itself. */
    int all = (e->o.measure & PI_MEASURE_ALL_ROWS) != 0;
    uint32_t c0 = 0, crows = ch;
    if (p->row_last > p->row_first && !all) {
        c0 = p->row_first / 2;
        crows = (p->row_last - p->row_first) / 2;   /* row_last: exclusive */
    }
    struct band b1 = {p1, c0, crows};
    pass(e, c, 0, c->fb, cw, ch, c0, crows, &b1, 1, NULL);
    /* Pass 2: the bars (block rows wholly outside the active rows) with the
     * black pipeline, the rest with the full one: one render pass. The bars
     * are the black pixel pair everywhere but in the rows carrying metadata
     * packets: the tile buffer is cleared to it (free), and only the packet
     * rows of the bars are drawn. */
    uint32_t a0 = p->active[1] / 2, a1 = (p->active[3] + 1) / 2;
    if (all || a1 <= a0 || a1 > ch)
        a0 = 0, a1 = ch;
    uint32_t prow = (c->pmap->pk[3] + 1) / 2;   /* block rows the packets reach */
    struct band b[3] = {{pb, 0, a0 < prow ? a0 : prow}, {p2, a0, a1 - a0},
                        {pb, a1, prow > a1 ? (prow < ch ? prow : ch) - a1 : 0}};
    if (e->o.measure & PI_MEASURE_DRAW_BARS)
        b[0].rows = a0, b[2].rows = ch - a1;
    VkClearValue black;
    for (int l = 0; l < 2; l++)
        black.color.uint32[l] = PI_PACK(p->black[0], l ? p->black[2] : p->black[1]);
    black.color.uint32[2] = black.color.uint32[3] = 0;
    pass(e, c, 1, o->fb, cw, ch, 0, ch, b, 3, &black);
    if (o->copy_buf)
        for (int half = 0; half < 2; half++) {
            VkBufferImageCopy cp = {.bufferOffset = (VkDeviceSize)half * o->stride, .bufferRowLength = o->stride / 4,
                                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {cw, ch, 1}};
            if (half) {   /* the halves interleave rows: disjoint, but syncval sees overlapping ranges */
                VkBufferMemoryBarrier bb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = o->copy_buf,
                                            .size = VK_WHOLE_SIZE};
                vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                                     1, &bb, 0, NULL);
            }
            vkCmdCopyImageToBuffer(c->cmd, o->half[half].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o->copy_buf, 1, &cp);
        }
    ownership(e, c, o, 0);
    VKC(e, vkEndCommandBuffer(c->cmd));
    return 0;
}

/* A sync_file signalled: timeout 0 checks, -1 waits. */
static int fd_signalled(int fd, int timeout)
{
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    int r;
    while ((r = poll(&pf, 1, timeout)) < 0 && errno == EINTR)
        ;
    return r != 0;   /* an error: nothing to wait for */
}

static int submit(struct pi_engine *e, struct ctx *c, int acquire_fd, int *release_fd)
{
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c->cmd};
    /* An acquire fence already signalled (the usual case: the output's last
     * scanout ended frames ago) isn't imported: waiting on a semaphore costs
     * v3dv the overlap of this frame's work with the last's. */
    if (acquire_fd >= 0 && fd_signalled(acquire_fd, 0)) {
        close(acquire_fd);
        acquire_fd = -1;
    }
    if (acquire_fd >= 0) {
        VkImportSemaphoreFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                                          .semaphore = c->acquire, .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                          .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, .fd = acquire_fd};
        if (e->sem_fd && e->import_sem_fd(e->dev, &imp) == VK_SUCCESS) {
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &c->acquire;
            si.pWaitDstStageMask = &stage;
        } else {
            fd_signalled(acquire_fd, -1);
            close(acquire_fd);
        }
    }
    int export = release_fd && e->sem_fd;
    if (export) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &c->release;
    }
    double s0 = now_ms();
    VKC(e, vkQueueSubmit(e->queue, 1, &si, c->fence));
    if (now_ms() - s0 > 1.0)
        fprintf(stderr, "XP submit took %.1f ms (export %d)\n", now_ms() - s0, export);
    if (release_fd)
        *release_fd = -1;
    if (export) {
        VkSemaphoreGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = c->release,
                                      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        double x0 = now_ms();
        if (e->get_sem_fd(e->dev, &gi, release_fd) != VK_SUCCESS)
            *release_fd = -1;
        double x1 = now_ms() - x0;
        if (x1 > 1.0)
            fprintf(stderr, "XP export took %.1f ms\n", x1);
    }
    return 0;
}

int pi_render(struct pi_engine *e, const struct pi_frame *f, struct pi_output *o, int *release_fd)
{
    if (release_fd)
        *release_fd = -1;
    int acquire_fd = f ? f->acquire_fd : -1;
    int rc = PI_E_INVALID;
    if (!e->open || !f || !f->bl || !o || !f->rpu || o->w != e->w || o->h != e->h)
        goto out;
    rc = PI_E_UNSUPPORTED_FORMAT;
    if ((uint32_t)f->bl->width != e->w || (uint32_t)f->bl->height != e->h ||
        (f->el && ((uint32_t)f->el->width > e->w || (uint32_t)f->el->height > e->h || f->el->width < 4 ||
                   f->el->height < 4)))
        goto out;
    struct ctx *c = &e->c[e->next];
    ctx_retire(e, c, 1);
    imports_trim(e);

    /* plan */
    double t0 = now_ms();
    dvvk_plan_in in = {.width = e->w, .height = e->h, .x = 0, .y = 0, .dw = e->w, .dh = e->h,
                       .chroma_top_left = (uint32_t)f->chroma_top_left, .has_el = f->el != NULL, .el_scaler = 2,
                       .fel = 1, .refresh = (uint32_t)e->first, .pts = f->pts_us, .white_nits = 203.0,
                       .cb1_contract = e->o.filters == PI_FILTERS_CB1};
    dvvk_plan *p = e->plan;
    int st = dvvk_plan_frame_state(&in, NULL, e->state, f->rpu, f->rpu_size, p);
    if (st) {
        WARN_ONCE(e, W_PLANNER, "the planner refuses the frame: dv_status %d", st);
        rc = st == -1 ? PI_E_BAD_RPU : PI_E_UNSUPPORTED_PLAN;
        goto out;
    }
    struct route rt;
    if (!route_of(e, p, f->el != NULL, &rt)) {
        WARN_ONCE(e, W_PLAN, "plan outside this engine (route %u equal %u el filter %u pack %u)", p->route, p->equal,
                  p->nearest, p->pack);
        rc = PI_E_UNSUPPORTED_PLAN;
        goto out;
    }
    if (e->first)
        logf_(e, PI_LOG_DEBUG, "route %u; active area x %u..%u, y %u..%u; row window %u..%u", p->route, p->active[0],
              p->active[2], p->active[1], p->active[3], p->row_first, p->row_last);
    prepare(e, p, f->el != NULL);

    /* inputs and tables */
    struct params *q = c->pmap;
    memcpy(q, &e->tmpl, sizeof(*q));
    if ((rc = bind_inputs(e, c, f->bl, f->el ? f->el : f->bl, q)))
        goto release;
    frame_params(e, p, f->el, q);
    if (c->tables_serial != e->tables_serial) {
        memcpy(c->tmap, e->tables, T_WORDS * 4);
        memcpy(c->rmap, p->reshape, sizeof(p->reshape));
        c->tables_serial = e->tables_serial;
    }
    c->stats.plan_ms = now_ms() - t0;
    c->stats.pass_ms[0] = c->stats.pass_ms[1] = 0;

    /* pipelines */
    uint32_t k1[K1_N], k2[K2_N], kb[K2_N] = {[K2_BARS] = 1};
    keys(e, p, k1, k2);
    VkPipeline p1 = build(e, 0, k1), p2 = build(e, 1, k2), pb = build(e, 1, kb);
    if (!p1 || !p2 || !pb) {
        WARN_ONCE(e, W_PIPELINE, "a pipeline failed to build");
        rc = PI_E_VULKAN;
        goto release;
    }
    prewarm_family(e, k1);

    /* record and submit */
    e->serial++;
    if ((rc = record(e, c, p, o, p1, p2, pb)))
        goto release;
    rc = submit(e, c, acquire_fd, release_fd);
    acquire_fd = -1;   /* submit takes it */
    if (rc)
        goto release;
    c->busy = 1;
    c->serial = e->serial;
    c->out = o;
    c->bl = av_frame_clone(f->bl);   /* the GPU reads them until the fence */
    c->el = f->el ? av_frame_clone(f->el) : NULL;
    o->ctx = (int)e->next;
    o->serial = e->serial;
    e->next = (e->next + 1) % PI_FRAMES_IN_FLIGHT;
    e->first = 0;
    return 0;
release:
    for (int i = 0; i < 4; i++)
        if (c->in[i]) {
            c->in[i]->refs--;
            c->in[i] = NULL;
        }
out:
    if (acquire_fd >= 0)
        close(acquire_fd);
    return rc;
}
