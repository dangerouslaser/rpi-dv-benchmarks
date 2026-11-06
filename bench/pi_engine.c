/* The Pi FEL engine (pi_engine.h): two fragment passes per frame, each one
 * render pass (one V3D render job), reading the decoder's SAND128 dmabufs
 * directly as texel buffers and rendering the tunnel frame into the linear
 * output dmabuf itself.
 *   P1  cw x ch   reconstructed 4:2:0 chroma (RG16UI) + the EL luma (R16UI)
 *   P2  cw x ch   the 4:2:2 filter + luma + pack + metadata, a 2x2 block per
 *                 fragment -> the output's even and odd rows (two RG32UI
 *                 images over the same dmabuf)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "pi_engine.h"

#include <libavutil/hwcontext_drm.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "dvvk_plan.h"

static const uint32_t spv_vert[] =
#include "pi/fullscreen.vert.h"
    ;
static const uint32_t spv_p1[] =
#include "pi/p1_chroma.frag.h"
    ;
static const uint32_t spv_p3[] =
#include "pi/p3_pack.frag.h"
    ;

#define VKC(x)                                                                         \
    do {                                                                               \
        VkResult r_ = (x);                                                             \
        if (r_ != VK_SUCCESS) {                                                        \
            fprintf(stderr, "pi: %s:%d: %s = %d\n", __FILE__, __LINE__, #x, (int)r_); \
            return -1;                                                                 \
        }                                                                              \
    } while (0)

enum { T_MAP = 0, T_RES = 1024, T_CMAP = 4096, T_LUTF = 6144, T_LUT64 = 12288, T_WORDS = 24576 };
enum { NB = 11 };   /* descriptor bindings */

struct params {   /* common.glsl Params, std140 */
    uint32_t size[4], bl[4], el[4], el_size[4], sel[4], sel2[4], active[4], black[4], pk[4];
    uint32_t lo[2][4], hi[2][4], const64[2][4];
    int32_t c_lo[6][4], c_hi[6][4], c32[6][4];
    float cf[6][4], cconst[4], inv[4];
    uint32_t packets[160];
};

struct image {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
};

struct import {
    dev_t dev;
    ino_t ino;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkBufferView view;
    struct import *next;
};

struct slot {
    struct image c420, ely, out[2];      /* out: the output's even, odd rows (direct) or copy sources */
    VkBuffer out_buf;                    /* copy mode: the output dmabuf as a buffer */
    VkDeviceMemory out_mem;
    uint32_t stride;
    VkFramebuffer fb[3];
    VkBuffer tables, ubo;
    VkDeviceMemory tables_mem, ubo_mem;
    uint32_t *tmap;
    struct params *pmap;
    VkBufferView tview, tview64, tview128;
    VkDescriptorSet ds;
    VkCommandBuffer cmd;
    VkFence fence;
    int busy, has_output;
    VkQueryPool qp;
    double pass_ms[3], plan_ms;
};

struct pi_engine {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties mp;
    PFN_vkGetMemoryFdPropertiesKHR get_fd_props;
    int modifiers, profile;
    double ts_period;
    char name[300];
    VkSampler sampler;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkRenderPass rp[3];
    struct variant {
        uint32_t key[7];
        VkPipeline p;
    } v1[16], v2[8];
    int n1, n2;
    struct slot s[3];
    struct import *imports;
    dvvk_plan_state *state;
    dvvk_plan *plan;
    enum pi_mmr_mode mode;
    int contract, warned;
    uint32_t ablate;   /* PI_ABLATE: measurement only */
    uint32_t w, h;
};

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint32_t memtype(struct pi_engine *e, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < e->mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (e->mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    for (uint32_t i = 0; i < e->mp.memoryTypeCount; i++)
        if (bits & (1u << i))
            return i;
    return 0;
}

static int host_buffer(struct pi_engine *e, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b,
                       VkDeviceMemory *m, void **map)
{
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VKC(vkCreateBuffer(e->dev, &ci, NULL, b));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(e->dev, *b, &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                               .memoryTypeIndex = memtype(e, r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKC(vkAllocateMemory(e->dev, &ai, NULL, m));
    VKC(vkBindBufferMemory(e->dev, *b, *m, 0));
    VKC(vkMapMemory(e->dev, *m, 0, VK_WHOLE_SIZE, 0, map));
    return 0;
}

static int image(struct pi_engine *e, struct image *im, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(vkCreateImage(e->dev, &ci, NULL, &im->img));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(e->dev, im->img, &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                               .memoryTypeIndex = memtype(e, r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKC(vkAllocateMemory(e->dev, &ai, NULL, &im->mem));
    VKC(vkBindImageMemory(e->dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VKC(vkCreateImageView(e->dev, &vi, NULL, &im->view));
    return 0;
}

static int import_memory(struct pi_engine *e, int fd, VkMemoryRequirements *r, const void *dedicated,
                         VkDeviceMemory *m)
{
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VKC(e->get_fd_props(e->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));
    int dupfd = dup(fd);
    if (dupfd < 0)
        return -1;
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = dedicated,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dupfd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                               .allocationSize = r->size,
                               .memoryTypeIndex = memtype(e, r->memoryTypeBits & fp.memoryTypeBits, 0)};
    VkResult res = vkAllocateMemory(e->dev, &ai, NULL, m);
    if (res != VK_SUCCESS) {
        close(dupfd);
        fprintf(stderr, "pi: dmabuf import (%llu bytes) failed: %d\n", (unsigned long long)r->size, (int)res);
        return -1;
    }
    return 0;
}

/* A decoder dmabuf as an R32UI texel buffer, once per buffer (by inode). */
static struct import *import_dmabuf(struct pi_engine *e, int fd)
{
    struct stat st;
    if (fstat(fd, &st))
        return NULL;
    for (struct import *i = e->imports; i; i = i->next)
        if (i->dev == st.st_dev && i->ino == st.st_ino)
            return i;
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size <= 0)
        return NULL;
    struct import *i = calloc(1, sizeof(*i));
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
        fprintf(stderr, "pi: buffer needs %llu bytes, the dmabuf has %lld\n", (unsigned long long)r.size,
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
    return i;
fail:
    if (i->buf)
        vkDestroyBuffer(e->dev, i->buf, NULL);
    if (i->mem)
        vkFreeMemory(e->dev, i->mem, NULL);
    free(i);
    return NULL;
}

static int render_pass(struct pi_engine *e, int n, const VkFormat *formats, VkImageLayout final, VkRenderPass *rp)
{
    VkAttachmentDescription a[2];
    VkAttachmentReference ref[2];
    for (int i = 0; i < n; i++) {
        a[i] = (VkAttachmentDescription){.format = formats[i], .samples = VK_SAMPLE_COUNT_1_BIT,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final};
        ref[i] = (VkAttachmentReference){(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = (uint32_t)n,
                               .pColorAttachments = ref};
    /* Its output is read by the next passes' fragment shaders (or copied, or
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
    VKC(vkCreateRenderPass(e->dev, &ci, NULL, rp));
    return 0;
}

static int pipeline(struct pi_engine *e, const uint32_t *spv, size_t bytes, VkRenderPass rp, int targets,
                    const uint32_t *spec, uint32_t nspec, VkPipeline *p)
{
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = spv};
    VkShaderModule fs, vs;
    VKC(vkCreateShaderModule(e->dev, &mi, NULL, &fs));
    mi.codeSize = sizeof(spv_vert);
    mi.pCode = spv_vert;
    VKC(vkCreateShaderModule(e->dev, &mi, NULL, &vs));
    VkSpecializationMapEntry me[8];
    for (uint32_t i = 0; i < nspec; i++)
        me[i] = (VkSpecializationMapEntry){i, 4 * i, 4};
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
                                               .attachmentCount = (uint32_t)targets, .pAttachments = cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                       .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cbs, .pDynamicState = &ds, .layout = e->pl,
                                       .renderPass = rp};
    VKC(vkCreateGraphicsPipelines(e->dev, VK_NULL_HANDLE, 1, &ci, NULL, p));
    vkDestroyShaderModule(e->dev, fs, NULL);
    vkDestroyShaderModule(e->dev, vs, NULL);
    return 0;
}

static int has_ext(VkPhysicalDevice p, const char *name)
{
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(p, NULL, &n, NULL);
    VkExtensionProperties *x = calloc(n, sizeof(*x));
    vkEnumerateDeviceExtensionProperties(p, NULL, &n, x);
    int found = 0;
    for (uint32_t i = 0; i < n && !found; i++)
        found = !strcmp(x[i].extensionName, name);
    free(x);
    return found;
}

static int create(struct pi_engine *e, const char *want)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pi-fel",
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VKC(vkCreateInstance(&ici, NULL, &e->inst));
    uint32_t n = 8;
    VkPhysicalDevice devs[8];
    VKC(vkEnumeratePhysicalDevices(e->inst, &n, devs));
    VkPhysicalDeviceProperties props;
    for (int pass = 0; pass < 2 && !e->phys; pass++)
        for (uint32_t i = 0; i < n && !e->phys; i++) {
            vkGetPhysicalDeviceProperties(devs[i], &props);
            if (want ? strstr(props.deviceName, want) != NULL
                     : pass == 1 || props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
                e->phys = devs[i];
        }
    if (!e->phys) {
        fprintf(stderr, "pi: no Vulkan device%s%s\n", want ? " matching " : "", want ? want : "");
        return -1;
    }
    vkGetPhysicalDeviceProperties(e->phys, &props);
    vkGetPhysicalDeviceMemoryProperties(e->phys, &e->mp);
    e->ts_period = props.limits.timestampPeriod;
    const char *exts[4] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    uint32_t next = 2;
    for (uint32_t i = 0; i < next; i++)
        if (!has_ext(e->phys, exts[i])) {
            fprintf(stderr, "pi: %s lacks %s\n", props.deviceName, exts[i]);
            return -1;
        }
    if (has_ext(e->phys, "VK_EXT_image_drm_format_modifier") && !getenv("PI_COPY_OUTPUT")) {
        exts[next++] = "VK_EXT_image_drm_format_modifier";
        e->modifiers = 1;
    }
    uint32_t nq = 8, qfam = UINT32_MAX;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(e->phys, &nq, qf);
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            qfam = i;
    e->profile = getenv("PI_PROFILE") && qf[qfam].timestampValidBits;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    VKC(vkCreateDevice(e->phys, &dci, NULL, &e->dev));
    vkGetDeviceQueue(e->dev, qfam, 0, &e->queue);
    e->get_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(e->dev, "vkGetMemoryFdPropertiesKHR");
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam};
    VKC(vkCreateCommandPool(e->dev, &pci, NULL, &e->pool));
    snprintf(e->name, sizeof(e->name), "%s, MMR %s, output %s", props.deviceName,
             e->mode == PI_MMR_FLOAT ? "float" : e->mode == PI_MMR_EXACT32 ? "exact (32-bit pieces)"
                                                                             : "exact (emulated 64-bit)",
             e->modifiers ? "rendered in place" : "copied");

    VkSamplerCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VKC(vkCreateSampler(e->dev, &smi, NULL, &e->sampler));
    static const VkDescriptorType types[NB] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,       VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER};
    VkDescriptorSetLayoutBinding b[NB];
    VkDescriptorPoolSize sizes[NB];
    for (int i = 0; i < NB; i++) {
        b[i] = (VkDescriptorSetLayoutBinding){(uint32_t)i, types[i], 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        sizes[i] = (VkDescriptorPoolSize){types[i], 3};
    }
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = NB, .pBindings = b};
    VKC(vkCreateDescriptorSetLayout(e->dev, &dli, NULL, &e->dsl));
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &e->dsl};
    VKC(vkCreatePipelineLayout(e->dev, &pli, NULL, &e->pl));
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3,
                                      .poolSizeCount = NB, .pPoolSizes = sizes};
    VKC(vkCreateDescriptorPool(e->dev, &dpi, NULL, &e->dpool));

    const VkFormat f1[2] = {VK_FORMAT_R16G16_UINT, VK_FORMAT_R16_UINT},
                   f2[2] = {VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_UINT};
    if (render_pass(e, 2, f1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &e->rp[0]) ||
        render_pass(e, 2, f2, e->modifiers ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    &e->rp[1]))
        return -1;
    e->ablate = getenv("PI_ABLATE") ? (uint32_t)atoi(getenv("PI_ABLATE")) : 0;

    e->w = 3840;
    e->h = 2160;
    const uint32_t cw = e->w / 2, ch = e->h / 2;
    const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int k = 0; k < 3; k++) {
        struct slot *s = &e->s[k];
        if (image(e, &s->c420, VK_FORMAT_R16G16_UINT, cw, ch, rt) || image(e, &s->ely, VK_FORMAT_R16_UINT, cw, ch, rt))
            return -1;
        VkImageView v1[2] = {s->c420.view, s->ely.view};
        VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = e->rp[0],
                                       .attachmentCount = 2, .pAttachments = v1, .width = cw, .height = ch,
                                       .layers = 1};
        VKC(vkCreateFramebuffer(e->dev, &fbi, NULL, &s->fb[0]));

        void *map;
        if (host_buffer(e, T_WORDS * 4, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, &s->tables, &s->tables_mem, &map))
            return -1;
        s->tmap = map;
        if (host_buffer(e, sizeof(struct params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &s->ubo, &s->ubo_mem, &map))
            return -1;
        s->pmap = map;
        VkBufferViewCreateInfo bvi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = s->tables,
                                      .format = VK_FORMAT_R32_UINT, .range = VK_WHOLE_SIZE};
        VKC(vkCreateBufferView(e->dev, &bvi, NULL, &s->tview));
        bvi.format = VK_FORMAT_R32G32_UINT;
        VKC(vkCreateBufferView(e->dev, &bvi, NULL, &s->tview64));
        bvi.format = VK_FORMAT_R32G32B32A32_UINT;
        VKC(vkCreateBufferView(e->dev, &bvi, NULL, &s->tview128));
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorPool = e->dpool, .descriptorSetCount = 1, .pSetLayouts = &e->dsl};
        VKC(vkAllocateDescriptorSets(e->dev, &dai, &s->ds));
        VkDescriptorBufferInfo ub = {s->ubo, 0, VK_WHOLE_SIZE};
        VkDescriptorImageInfo ii[3] = {{e->sampler, s->c420.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {e->sampler, s->ely.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {e->sampler, s->c420.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet wr[7] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 10, .descriptorCount = 1,
             .descriptorType = types[10], .pTexelBufferView = &s->tview128},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = types[0], .pBufferInfo = &ub},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = types[1], .pTexelBufferView = &s->tview},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = types[2], .pTexelBufferView = &s->tview64},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 7, .descriptorCount = 1,
             .descriptorType = types[7], .pImageInfo = &ii[0]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 8, .descriptorCount = 1,
             .descriptorType = types[8], .pImageInfo = &ii[1]},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds, .dstBinding = 9, .descriptorCount = 1,
             .descriptorType = types[9], .pImageInfo = &ii[2]}};
        vkUpdateDescriptorSets(e->dev, 7, wr, 0, NULL);
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = e->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
        VKC(vkAllocateCommandBuffers(e->dev, &cai, &s->cmd));
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKC(vkCreateFence(e->dev, &fci, NULL, &s->fence));
        if (e->profile) {
            VkQueryPoolCreateInfo qpi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                         .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
            VKC(vkCreateQueryPool(e->dev, &qpi, NULL, &s->qp));
        }
    }
    return 0;
}

struct pi_engine *pi_create(const char *device, enum pi_mmr_mode mode, int cb1_contract)
{
    struct pi_engine *e = calloc(1, sizeof(*e));
    e->mode = mode;
    e->contract = cb1_contract;
    e->plan = calloc(1, sizeof(dvvk_plan));
    e->state = dvvk_plan_state_new();
    if (!e->plan || !e->state || create(e, device)) {
        pi_destroy(e);
        return NULL;
    }
    return e;
}

/* Output row parity `half` (0 even rows, 1 odd rows) of the dmabuf: a linear
 * RG32UI image of w/2 x h/2 pixel pairs, rows 2 * stride apart, from byte
 * half * stride. The pack pass renders both as its two colour attachments:
 * one fragment, a 2 x 2 block of pixels. */
static int output_half(struct pi_engine *e, struct slot *s, int fd, uint32_t stride, int half)
{
    struct image *o = &s->out[half];
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    if (e->modifiers) {
        VkSubresourceLayout plane = {.offset = (VkDeviceSize)half * stride, .rowPitch = 2 * (VkDeviceSize)stride};
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .pNext = &ext,
            .drmFormatModifier = 0 /* DRM_FORMAT_MOD_LINEAR */, .drmFormatModifierPlaneCount = 1,
            .pPlaneLayouts = &plane};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &mod,
                                .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32_UINT,
                                .extent = {e->w / 2, e->h / 2, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VKC(vkCreateImage(e->dev, &ci, NULL, &o->img));
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(e->dev, o->img, &r);
        VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                             .image = o->img};
        if (import_memory(e, fd, &r, &ded, &o->mem))
            return -1;
        VKC(vkBindImageMemory(e->dev, o->img, o->mem, 0));
    } else if (image(e, o, VK_FORMAT_R32G32_UINT, e->w / 2, e->h / 2,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return -1;
    if (!o->view) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = o->img,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R32G32_UINT,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKC(vkCreateImageView(e->dev, &vi, NULL, &o->view));
    }
    return 0;
}

int pi_set_output(struct pi_engine *e, unsigned slot, int fd, uint32_t stride)
{
    struct slot *s = &e->s[slot];
    s->stride = stride;
    if (output_half(e, s, fd, stride, 0) || output_half(e, s, fd, stride, 1))
        return -1;
    if (!e->modifiers) {
        /* Copy mode: our own images, copied into the dmabuf. */
        VkExternalMemoryBufferCreateInfo bext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                 .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &bext,
                                  .size = (VkDeviceSize)stride * e->h, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKC(vkCreateBuffer(e->dev, &bci, NULL, &s->out_buf));
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(e->dev, s->out_buf, &r);
        if (import_memory(e, fd, &r, NULL, &s->out_mem))
            return -1;
        VKC(vkBindBufferMemory(e->dev, s->out_buf, s->out_mem, 0));
    }
    VkImageView views[2] = {s->out[0].view, s->out[1].view};
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = e->rp[1],
                                   .attachmentCount = 2, .pAttachments = views, .width = e->w / 2,
                                   .height = e->h / 2, .layers = 1};
    VKC(vkCreateFramebuffer(e->dev, &fbi, NULL, &s->fb[1]));
    s->has_output = 1;
    return 0;
}

/* ---- per frame ---------------------------------------------------------------- */

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

/* (a * b) >> 20, as the shaders' feature_product. */
static uint64_t fprod(uint64_t a, uint64_t b) { return ((a * b) >> 20) & 0xffffffffu; }

/* The float MMR (mode 0) of channel c: constants, cross coefficients and
 * the univariate LUTs, all / 2^(shift + 4), for any MMR kind. */
static void float_mmr(const dvvk_plan *p, unsigned c, struct params *q, float *lutf)
{
    const uint32_t *w = p->mmr_params[c];
    unsigned kind = p->chroma_kind[c], order = w[0];
    double scale = ldexp(1.0, -(int)(w[1] + 4));
    /* coefficient (feature k, order o): kind 3 int32, 2 int64, 1 int128 */
    int n = kind == DVVK_CHROMA_MMR64_LUT ? 1 : kind == DVVK_CHROMA_MMR96 ? 2 : 4;
    double constant = kind == DVVK_CHROMA_MMR64_LUT ? words_value(w + 8, 2)
                    : kind == DVVK_CHROMA_MMR96     ? words_value(w + 8, 3)
                                                    : words_value(w + 8, 4);
    q->cconst[c] = (float)(constant * scale);
#define COEF(k, o) words_value(w + 12 + 28 * (o) + 4 * (k), n)
    for (unsigned o = 0; o < 3; o++)
        for (unsigned k = 0; k < 4; k++)
            q->cf[c * 3 + o][k] = o < order && !(kind == DVVK_CHROMA_MMR128 && o == 0 && order == 0)
                                      ? (float)(COEF(k + 3, o) * scale)
                                      : 0.0f;
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

static int bind_inputs(struct pi_engine *e, struct slot *s, const AVFrame *bl, const AVFrame *el, struct params *q)
{
    const AVFrame *f[2] = {bl, el};
    VkBufferView views[4];
    for (int l = 0; l < 2; l++) {
        const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f[l]->data[0];
        const AVDRMLayerDescriptor *ly = &d->layers[0];
        uint64_t mod = d->objects[0].format_modifier;
        if (d->nb_layers != 1 || ly->nb_planes != 2 || (mod >> 56) != 0x07 || (mod & 0xff) != 4) {
            fprintf(stderr, "pi: input isn't SAND128 (one layer, two planes)\n");
            return -1;
        }
        uint32_t *o = l ? q->el : q->bl;
        static int shown;
        if (getenv("PI_DEBUG") && shown < 2) {
            shown++;
            fprintf(stderr, "pi: %s %dx%d objects %d", l ? "EL" : "BL", f[l]->width, f[l]->height, d->nb_objects);
            for (int k = 0; k < d->nb_objects; k++)
                fprintf(stderr, " [fd size %zu mod %llx]", d->objects[k].size,
                        (unsigned long long)d->objects[k].format_modifier);
            for (int k = 0; k < ly->nb_planes; k++)
                fprintf(stderr, " plane%d obj %d off %td pitch %td", k, ly->planes[k].object_index,
                        ly->planes[k].offset, ly->planes[k].pitch);
            fprintf(stderr, "\n");
        }
        for (int p = 0; p < 2; p++) {
            struct import *im = import_dmabuf(e, d->objects[ly->planes[p].object_index].fd);
            if (!im)
                return -1;
            views[l * 2 + p] = im->view;
            if (ly->planes[p].offset & 3)
                return -1;
            o[p] = (uint32_t)(ly->planes[p].offset / 4);
        }
        /* Column heights: the modifier's, else (the Pi decoder: 0 there, luma
         * and chroma in separate buffers) each plane's pitch, as yblod's
         * import (interop/va.rs) and FFmpeg's SAND code take them. */
        uint32_t column = (uint32_t)((mod >> 8) & 0xffffffffu);
        o[2] = column ? column : (uint32_t)ly->planes[0].pitch;
        o[3] = column ? column : (uint32_t)ly->planes[1].pitch;
    }
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < 4; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->ds,
                                       .dstBinding = 3u + (uint32_t)i, .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                       .pTexelBufferView = &views[i]};
    vkUpdateDescriptorSets(e->dev, 4, wr, 0, NULL);
    return 0;
}

static void pass(struct pi_engine *e, struct slot *s, int i, VkPipeline p, uint32_t w, uint32_t h)
{
    VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = e->rp[i],
                                 .framebuffer = s->fb[i], .renderArea = {{0, 0}, {w, h}}};
    vkCmdBeginRenderPass(s->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(s->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->pl, 0, 1, &s->ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(s->cmd, 0, 1, &vp);
    vkCmdSetScissor(s->cmd, 0, 1, &sc);
    vkCmdDraw(s->cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s->cmd);
    if (s->qp)
        vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->qp, (uint32_t)i + 1);
}

/* The pipeline for specialization `key` (constant ids first .. first + n - 1),
 * made the first time a stream needs it. */
static VkPipeline variant(struct pi_engine *e, struct variant *v, int *count, int max, const uint32_t *spv, size_t bytes,
                          VkRenderPass rp, int targets, const uint32_t *key, uint32_t n, uint32_t first)
{
    for (int i = 0; i < *count; i++)
        if (!memcmp(v[i].key, key, n * 4))
            return v[i].p;
    if (*count == max)
        return VK_NULL_HANDLE;
    uint32_t spec[8] = {0};
    memcpy(spec + first, key, n * 4);   /* ids 0 .. first - 1: their defaults' values */
    if (first)
        spec[0] = PI_MMR_FLOAT;         /* p2 has no constant 0; any value */
    double t0 = now_ms();
    if (pipeline(e, spv, bytes, rp, targets, spec, n + first, &v[*count].p))
        return VK_NULL_HANDLE;
    memcpy(v[*count].key, key, n * 4);
    if (getenv("PI_DEBUG"))
        fprintf(stderr, "pi: pipeline variant %d made in %.0f ms\n", *count, now_ms() - t0);
    return v[(*count)++].p;
}

int pi_submit(struct pi_engine *e, const AVFrame *bl, const AVFrame *el, const void *rpu, size_t rpu_size,
              int chroma_top_left, int64_t pts_us, int first, unsigned slot)
{
    struct slot *s = &e->s[slot];
    if (!s->has_output || pi_wait(e, slot))
        return -1;
    if (!el)
        return 2;   /* this engine is the FEL route */
    uint32_t w = (uint32_t)bl->width, h = (uint32_t)bl->height;
    if (w != e->w || h != e->h)
        return 2;
    double t0 = now_ms();
    dvvk_plan_in in = {.width = w, .height = h, .x = 0, .y = 0, .dw = w, .dh = h,
                       .chroma_top_left = (uint32_t)chroma_top_left, .has_el = 1, .el_scaler = 2, .fel = 1,
                       .refresh = (uint32_t)first, .pts = pts_us, .white_nits = 203.0,
                       .cb1_contract = (uint32_t)e->contract};
    dvvk_plan *p = e->plan;
    int st = dvvk_plan_frame_state(&in, NULL, e->state, rpu, rpu_size, p);
    if (st)
        return st < 0 ? -st : st;
    if (p->route != DVVK_ROUTE_DUAL || p->equal || p->nearest != 2 || p->pack != DVVK_PACK_SITED ||
        p->placement[0] || p->placement[1] || p->placement[2] != w || p->placement[3] != h) {
        if (!e->warned++)
            fprintf(stderr, "pi: plan outside this engine (route %u equal %u el filter %u pack %u)\n", p->route,
                    p->equal, p->nearest, p->pack);
        return 2;
    }
    struct params *q = s->pmap;
    memset(q, 0, sizeof(*q));
    if (bind_inputs(e, s, bl, el, q))
        return -1;
    q->size[0] = w;
    q->size[1] = h;
    q->size[2] = w / 2;
    q->size[3] = h / 2;
    q->el_size[0] = (uint32_t)el->width / 2;
    q->el_size[1] = (uint32_t)el->height / 2;
    q->sel[0] = p->chroma_phase;
    q->sel[1] = p->dual;
    q->sel[2] = p->chroma_kind[0];
    q->sel[3] = p->chroma_kind[1];
    q->sel2[0] = p->mmr_params[0][0];
    q->sel2[1] = p->mmr_params[1][0];
    q->sel2[2] = p->chroma_filter;
    q->sel2[3] = p->top_left;
    memcpy(q->active, p->active, sizeof(q->active));
    memcpy(q->black, p->black, sizeof(q->black));
    q->pk[0] = p->packet_count;
    q->pk[1] = p->mmr_params[0][1];
    q->pk[2] = p->mmr_params[1][1];
    q->pk[3] = (p->packet_count * 3072 + w - 1) / w;   /* rows the packets reach */
    q->inv[0] = 1.0f / (float)(w / 2);
    q->inv[1] = 1.0f / (float)(h / 2);
    memcpy(q->packets, p->packets, sizeof(p->packets));

    uint32_t *t = s->tmap;
    for (int i = 0; i < 1024; i++) {
        t[T_MAP + i] = p->mapping[i] + 8u;   /* the pack's rounding, folded in */
        t[T_CMAP + i] = p->chroma_mapping[0][i];
        t[T_CMAP + 1024 + i] = p->chroma_mapping[1][i];
    }
    memcpy(t + T_RES, p->residual, sizeof(p->residual));
    int exact = e->mode != PI_MMR_FLOAT;
    for (unsigned c = 0; c < 2; c++) {
        const uint32_t *pw = p->mmr_params[c];
        for (int k = 0; k < 3; k++) {
            q->lo[c][k] = pw[2 + 2 * k];
            q->hi[c][k] = pw[3 + 2 * k];
        }
        if (p->chroma_kind[c] == DVVK_CHROMA_POLY)
            continue;
        if (p->chroma_kind[c] != DVVK_CHROMA_MMR64_LUT && exact) {
            exact = 0;
            if (!e->warned++)
                fprintf(stderr, "pi: MMR kind %u has no exact mode here: float for these frames\n",
                        p->chroma_kind[c]);
        }
        q->const64[c][0] = pw[8];
        q->const64[c][1] = pw[9];
        for (unsigned o = 0; o < 3; o++)
            for (unsigned k = 0; k < 4; k++) {
                int32_t v = o < pw[0] ? (int32_t)pw[12 + 28 * o + 4 * (k + 3)] : 0;
                q->c32[c * 3 + o][k] = v;
                q->c_lo[c * 3 + o][k] = v & 0xffff;
                q->c_hi[c * 3 + o][k] = v >> 16;
            }
        float_mmr(p, c, q, (float *)(t + T_LUTF));
        if (p->chroma_kind[c] == DVVK_CHROMA_MMR64_LUT)
            for (int k = 0; k < 3; k++)
                for (int i = 0; i < 1024; i++) {
                    uint64_t v = (uint64_t)p->mmr_lut[c][k][i];
                    t[T_LUT64 + (k * 1024 + i) * 4 + c * 2] = (uint32_t)v;   /* Cb lo, hi, Cr lo, hi */
                    t[T_LUT64 + (k * 1024 + i) * 4 + c * 2 + 1] = (uint32_t)(v >> 32);
                }
    }
    s->plan_ms = now_ms() - t0;

    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKC(vkResetCommandBuffer(s->cmd, 0));
    VKC(vkBeginCommandBuffer(s->cmd, &bi));
    if (s->qp) {
        vkCmdResetQueryPool(s->cmd, s->qp, 0, 4);
        vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->qp, 0);
    }
    uint32_t k1[7] = {exact ? (uint32_t)e->mode : PI_MMR_FLOAT, p->chroma_phase, p->dual, p->chroma_kind[0],
                      p->chroma_kind[1], p->mmr_params[0][0], p->mmr_params[1][0]};
    uint32_t k2[3] = {p->chroma_filter != 0, p->top_left != 0, e->ablate};
    VkPipeline p1 = variant(e, e->v1, &e->n1, 16, spv_p1, sizeof(spv_p1), e->rp[0], 2, k1, 7, 0);
    VkPipeline p2 = variant(e, e->v2, &e->n2, 8, spv_p3, sizeof(spv_p3), e->rp[1], 2, k2, 3, 1);
    if (!p1 || !p2)
        return -1;
    pass(e, s, 0, p1, w / 2, h / 2);
    pass(e, s, 1, p2, w / 2, h / 2);
    if (!e->modifiers)
        for (int half = 0; half < 2; half++) {
            VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)half * s->stride, .bufferRowLength = s->stride / 4,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {w / 2, h / 2, 1}};
            vkCmdCopyImageToBuffer(s->cmd, s->out[half].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->out_buf, 1, &c);
        }
    VKC(vkEndCommandBuffer(s->cmd));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->cmd};
    VKC(vkQueueSubmit(e->queue, 1, &si, s->fence));
    s->busy = 1;
    return 0;
}

int pi_wait(struct pi_engine *e, unsigned slot)
{
    struct slot *s = &e->s[slot];
    if (!s->busy)
        return 0;
    VKC(vkWaitForFences(e->dev, 1, &s->fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(e->dev, 1, &s->fence));
    s->busy = 0;
    if (s->qp) {
        uint64_t ts[4];
        s->pass_ms[2] = 0;
        if (vkGetQueryPoolResults(e->dev, s->qp, 0, 3, 3 * sizeof(ts[0]), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (int i = 0; i < 2; i++)
                s->pass_ms[i] = (double)(ts[i + 1] - ts[i]) * e->ts_period / 1e6;
    }
    return 0;
}

int pi_done(struct pi_engine *e, unsigned slot)
{
    struct slot *s = &e->s[slot];
    return !s->busy || vkGetFenceStatus(e->dev, s->fence) == VK_SUCCESS;
}

void pi_times(struct pi_engine *e, unsigned slot, double pass_ms[3], double *plan_ms)
{
    memcpy(pass_ms, e->s[slot].pass_ms, sizeof(e->s[slot].pass_ms));
    *plan_ms = e->s[slot].plan_ms;
}

const char *pi_describe(struct pi_engine *e) { return e->name; }

void pi_destroy(struct pi_engine *e)
{
    if (!e)
        return;
    if (e->dev)
        vkDeviceWaitIdle(e->dev);
    /* The process exits right after: the device's objects go with it. */
    if (e->dev)
        vkDestroyDevice(e->dev, NULL);
    if (e->inst)
        vkDestroyInstance(e->inst, NULL);
    if (e->state)
        dvvk_plan_state_free(e->state);
    free(e->plan);
    free(e);
}
