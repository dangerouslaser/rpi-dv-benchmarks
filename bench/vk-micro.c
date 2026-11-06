/* vk-micro: is it Vulkan, or the engine's design? One synthetic 4K pass (read
 * P010-like 4:2:0, a reshape-like per-pixel computation, write RGBA8), the
 * same maths (micro/core.glsl) in every variant, on the same GPU:
 *
 *   vk-comp buf>buf   compute, storage buffers in and out (yblod's engine today)
 *   vk-comp tex>buf   compute, sampled images in, storage buffer out
 *   vk-comp tex>img   compute, sampled images in, storage image out
 *   vk-frag buf       fragment shader, storage buffers in, colour attachment out
 *   vk-frag tex       fragment shader, sampled images in, colour attachment out
 *   gl-frag tex       GLES fragment shader into an FBO (CB1/libplacebo's design)
 *   gl-comp tex>img   GLES compute, textures in, image out
 *
 * per WORK level (iterations of the per-pixel maths; 0 = just the traffic) in
 * float and in integer maths. Prints ms per pass and a checksum of the output
 * (every variant computes the same picture, so equal checksums mean equal work).
 *
 *   vk-micro [--size WxH] [--iters N] [--work 0,4,16] [--wg XxY] [--only SUBSTR]
 * MICRO_DEVICE=substr picks the Vulkan device (default: the first GPU).
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "micro/core_glsl.h"
static const uint32_t spv_comp_buf_buf[] =
#include "micro/comp_buf_buf.h"
    ;
static const uint32_t spv_comp_tex_buf[] =
#include "micro/comp_tex_buf.h"
    ;
static const uint32_t spv_comp_tex_img[] =
#include "micro/comp_tex_img.h"
    ;
static const uint32_t spv_frag_buf[] =
#include "micro/frag_buf.h"
    ;
static const uint32_t spv_frag_tex[] =
#include "micro/frag_tex.h"
    ;
static const uint32_t spv_vert[] =
#include "micro/vert.h"
    ;

#define CHECK(x)                                                                         \
    do {                                                                                 \
        VkResult r_ = (x);                                                               \
        if (r_ != VK_SUCCESS) {                                                          \
            fprintf(stderr, "vk-micro: %s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

static unsigned W = 3840, H = 2160, ITERS = 20, WGX = 8, WGY = 8;
static unsigned WORKS[8] = {0, 4, 16}, NWORK = 3;
static const char *ONLY;
static uint16_t *g_luma;   /* W*H */
static uint32_t *g_chroma; /* (W/2)*(H/2), Cb | Cr << 16 */

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint64_t hash32(const uint32_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static int wanted(const char *name) { return !ONLY || strstr(name, ONLY); }

static void report(const char *name, unsigned work, unsigned mode, double ms, uint64_t h)
{
    printf("%-16s work %2u %-5s %8.2f ms  %7.1f Mpix/s  %016llx\n", name, work, mode ? "int" : "float", ms,
           W * (double)H / ms / 1e3, (unsigned long long)h);
    fflush(stdout);
}

/* ---- Vulkan ------------------------------------------------------------------ */

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties memprops;

struct buf {
    VkBuffer b;
    VkDeviceMemory m;
    void *map;
};
struct img {
    VkImage i;
    VkDeviceMemory m;
    VkImageView v;
};

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if (bits & (1u << i))
            return i;
    return 0;
}

static struct buf mkbuf(VkDeviceSize size, VkBufferUsageFlags usage)
{
    struct buf b = {0};
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CHECK(vkCreateBuffer(dev, &ci, NULL, &b.b));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b.b, &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                               .memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &b.m));
    CHECK(vkBindBufferMemory(dev, b.b, b.m, 0));
    CHECK(vkMapMemory(dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.map));
    return b;
}

static struct img mkimg(VkFormat f, unsigned w, unsigned h, VkImageUsageFlags usage)
{
    struct img m = {0};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(dev, &ci, NULL, &m.i));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, m.i, &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                               .memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &m.m));
    CHECK(vkBindImageMemory(dev, m.i, m.m, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m.i,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(dev, &vi, NULL, &m.v));
    return m;
}

static VkCommandBuffer begin_cmd(void)
{
    VkCommandBuffer c;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(dev, &ai, &c));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(c, &bi));
    return c;
}

/* end, submit, wait; returns the wall time of submit + wait */
static double run_cmd(VkCommandBuffer c)
{
    CHECK(vkEndCommandBuffer(c));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    CHECK(vkCreateFence(dev, &fi, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c};
    double t0 = now_ms();
    CHECK(vkQueueSubmit(queue, 1, &si, f));
    CHECK(vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
    double t = now_ms() - t0;
    vkDestroyFence(dev, f, NULL);
    vkFreeCommandBuffers(dev, pool, 1, &c);
    return t;
}

static void layout(VkCommandBuffer c, VkImage i, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = i,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

static VkShaderModule module(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
                                   .pCode = code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

struct pc {
    uint32_t work, mode, width, height;
};

static void vulkan(void)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk-micro",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice devs[8], phys = VK_NULL_HANDLE;
    n = n > 8 ? 8 : n;
    vkEnumeratePhysicalDevices(inst, &n, devs);
    const char *want = getenv("MICRO_DEVICE");
    VkPhysicalDeviceProperties props;
    for (int pass = 0; pass < 2 && !phys; pass++)
        for (uint32_t i = 0; i < n && !phys; i++) {
            vkGetPhysicalDeviceProperties(devs[i], &props);
            if (want ? strstr(props.deviceName, want) != NULL
                     : pass == 1 || props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
                phys = devs[i];
        }
    if (!phys) {
        fprintf(stderr, "vk-micro: no Vulkan device\n");
        exit(1);
    }
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    printf("vulkan: %s\n", props.deviceName);
    uint32_t nq = 0, qfam = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qf[8];
    nq = nq > 8 ? 8 : nq;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++)
        if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            qfam = i;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci};
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    /* inputs: buffers (also the upload source for the images), images; outputs */
    size_t lbytes = (size_t)W * H * 2, cbytes = (size_t)(W / 2) * (H / 2) * 4, obytes = (size_t)W * H * 4;
    const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    struct buf lb = mkbuf(lbytes, su), cb = mkbuf(cbytes, su), ob = mkbuf(obytes, su), rb = mkbuf(obytes, su);
    memcpy(lb.map, g_luma, lbytes);
    memcpy(cb.map, g_chroma, cbytes);
    struct img li = mkimg(VK_FORMAT_R16_UINT, W, H, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct img ci = mkimg(VK_FORMAT_R16G16_UINT, W / 2, H / 2,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    struct img si = mkimg(VK_FORMAT_R8G8B8A8_UNORM, W, H,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct img ai = mkimg(VK_FORMAT_R8G8B8A8_UNORM, W, H,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkCommandBuffer c = begin_cmd();
    layout(c, li.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    layout(c, ci.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy lc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
    VkBufferImageCopy cc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W / 2, H / 2, 1}};
    vkCmdCopyBufferToImage(c, lb.b, li.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &lc);
    vkCmdCopyBufferToImage(c, cb.b, ci.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cc);
    layout(c, li.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    layout(c, ci.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    layout(c, si.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    layout(c, ai.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    run_cmd(c);

    /* one descriptor set with everything */
    VkSamplerCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                               .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    CHECK(vkCreateSampler(dev, &smi, NULL, &sampler));
    const VkDescriptorType types[6] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutBinding binds[6];
    VkDescriptorPoolSize sizes[6];
    for (int i = 0; i < 6; i++) {
        binds[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = types[i],
                                                  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_ALL};
        sizes[i] = (VkDescriptorPoolSize){types[i], 1};
    }
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 6, .pBindings = binds};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 6, .pPoolSizes = sizes};
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp,
                                       .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo bis[3] = {{lb.b, 0, VK_WHOLE_SIZE}, {cb.b, 0, VK_WHOLE_SIZE}, {ob.b, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo iis[3] = {{sampler, li.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {sampler, ci.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {VK_NULL_HANDLE, si.v, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[6];
    for (int i = 0; i < 6; i++) {
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
                                       .dstBinding = (uint32_t)i, .descriptorCount = 1, .descriptorType = types[i]};
        if (i == 0 || i == 1)
            wr[i].pBufferInfo = &bis[i];
        else if (i == 4)
            wr[i].pBufferInfo = &bis[2];
        else
            wr[i].pImageInfo = &iis[i == 5 ? 2 : i - 2];
    }
    vkUpdateDescriptorSets(dev, 6, wr, 0, NULL);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL, 0, sizeof(struct pc)};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));

    /* the fragment variants' render pass */
    VkAttachmentDescription att = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                   .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                   .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                               .pColorAttachments = &ar};
    VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                  .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
    VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                   .attachmentCount = 1, .pAttachments = &ai.v, .width = W, .height = H, .layers = 1};
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

    struct variant {
        const char *name;
        const uint32_t *spv;
        size_t bytes;
        int frag, out; /* out: 0 buffer, 1 storage image, 2 attachment */
    } vs[] = {
        {"vk-comp buf>buf", spv_comp_buf_buf, sizeof(spv_comp_buf_buf), 0, 0},
        {"vk-comp tex>buf", spv_comp_tex_buf, sizeof(spv_comp_tex_buf), 0, 0},
        {"vk-comp tex>img", spv_comp_tex_img, sizeof(spv_comp_tex_img), 0, 1},
        {"vk-frag buf", spv_frag_buf, sizeof(spv_frag_buf), 1, 2},
        {"vk-frag tex", spv_frag_tex, sizeof(spv_frag_tex), 1, 2},
    };
    VkShaderModule vert = module(spv_vert, sizeof(spv_vert));
    for (size_t v = 0; v < sizeof(vs) / sizeof(vs[0]); v++) {
        if (!wanted(vs[v].name))
            continue;
        VkShaderModule m = module(vs[v].spv, vs[v].bytes);
        VkPipeline pipe;
        double tc = now_ms();
        if (!vs[v].frag) {
            uint32_t wg[2] = {WGX, WGY};
            VkSpecializationMapEntry me[2] = {{0, 0, 4}, {1, 4, 4}};
            VkSpecializationInfo spi = {2, me, sizeof(wg), wg};
            VkComputePipelineCreateInfo cpi = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                          .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                          .pSpecializationInfo = &spi},
                .layout = pl};
            CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
        } else {
            VkPipelineShaderStageCreateInfo st[2] = {
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
                 .module = vert, .pName = "main"},
                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                 .module = m, .pName = "main"}};
            VkPipelineVertexInputStateCreateInfo vi = {.sType =
                                                           VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
            VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
            VkRect2D sc = {{0, 0}, {W, H}};
            VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                     .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                                     .pScissors = &sc};
            VkPipelineRasterizationStateCreateInfo rs = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
            VkPipelineMultisampleStateCreateInfo ms = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
            VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
            VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                       .attachmentCount = 1, .pAttachments = &cba};
            VkGraphicsPipelineCreateInfo gpi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                                .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                                .pInputAssemblyState = &ia, .pViewportState = &vps,
                                                .pRasterizationState = &rs, .pMultisampleState = &ms,
                                                .pColorBlendState = &cbs, .layout = pl, .renderPass = rp};
            CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
        }
        tc = now_ms() - tc;
        if (tc > 50)
            printf("%-16s (pipeline compile %.0f ms)\n", vs[v].name, tc);
        for (unsigned wi = 0; wi < NWORK; wi++)
            for (unsigned mode = 0; mode < 2; mode++) {
                struct pc pcv = {WORKS[wi], mode, W, H};
                double t[2];
                for (int rep = 0; rep < 2; rep++) { /* rep 0: warm-up */
                    unsigned iters = rep ? ITERS : 1;
                    c = begin_cmd();
                    VkPipelineBindPoint bp =
                        vs[v].frag ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
                    for (unsigned k = 0; k < iters; k++) {
                        if (vs[v].frag) {
                            VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                                         .renderPass = rp, .framebuffer = fb,
                                                         .renderArea = {{0, 0}, {W, H}}};
                            vkCmdBeginRenderPass(c, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                        }
                        vkCmdBindPipeline(c, bp, pipe);
                        vkCmdBindDescriptorSets(c, bp, pl, 0, 1, &ds, 0, NULL);
                        vkCmdPushConstants(c, pl, VK_SHADER_STAGE_ALL, 0, sizeof(pcv), &pcv);
                        if (vs[v].frag) {
                            vkCmdDraw(c, 3, 1, 0, 0);
                            vkCmdEndRenderPass(c);
                        } else {
                            vkCmdDispatch(c, (W + WGX - 1) / WGX, (H + WGY - 1) / WGY, 1);
                        }
                        /* each pass depends on the last (write after write), as a pipeline's passes do */
                        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
                        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                    }
                    t[rep] = run_cmd(c) / iters;
                }
                /* read the output back */
                const uint32_t *out = ob.map;
                if (vs[v].out) {
                    VkImage src = vs[v].out == 1 ? si.i : ai.i;
                    VkImageLayout l = vs[v].out == 1 ? VK_IMAGE_LAYOUT_GENERAL
                                                     : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    c = begin_cmd();
                    layout(c, src, l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    VkBufferImageCopy oc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                            .imageExtent = {W, H, 1}};
                    vkCmdCopyImageToBuffer(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.b, 1, &oc);
                    layout(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l);
                    run_cmd(c);
                    out = rb.map;
                }
                report(vs[v].name, WORKS[wi], mode, t[1], hash32(out, (size_t)W * H));
                memset(ob.map, 0, obytes);
                memset(rb.map, 0, obytes);
            }
        vkDestroyPipeline(dev, pipe, NULL);
        vkDestroyShaderModule(dev, m, NULL);
    }
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
}

/* ---- GLES -------------------------------------------------------------------- */

static GLuint shader(GLenum type, const char *const *parts, int n)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, n, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "vk-micro: GLES shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint program(GLuint a, GLuint b)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, a);
    if (b)
        glAttachShader(p, b);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "vk-micro: GLES link: %s\n", log);
        exit(1);
    }
    return p;
}

static const char gl_head[] = "#version 310 es\nprecision highp float;\nprecision highp int;\n"
                              "precision highp usampler2D;\nprecision highp image2D;\n"
                              "uniform uint work, mode, width, height;\n"
                              "uniform highp usampler2D luma_tex, chroma_tex;\n"
                              "vec3 fetch(uvec2 p) {\n"
                              "    uint y = texelFetch(luma_tex, ivec2(p), 0).r;\n"
                              "    uvec2 c = texelFetch(chroma_tex, ivec2(p >> 1), 0).rg;\n"
                              "    return vec3(float(y), float(c.x), float(c.y)) / 65535.0;\n}\n";

static void gles(void)
{
    EGLDisplay d = ((PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT"))(
        EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!d || !eglInitialize(d, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "vk-micro: EGL init failed\n");
        return;
    }
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (!ctx || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "vk-micro: EGL context failed\n");
        return;
    }
    printf("gles: %s\n", (const char *)glGetString(GL_RENDERER));
    GLuint tex[3];
    glGenTextures(3, tex);
    glBindTexture(GL_TEXTURE_2D, tex[0]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R16UI, W, H);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RED_INTEGER, GL_UNSIGNED_SHORT, g_luma);
    glBindTexture(GL_TEXTURE_2D, tex[1]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16UI, W / 2, H / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W / 2, H / 2, GL_RG_INTEGER, GL_UNSIGNED_SHORT, g_chroma);
    glBindTexture(GL_TEXTURE_2D, tex[2]);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[2], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "vk-micro: GLES framebuffer incomplete\n");
        return;
    }
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    static const char vs_src[] = "#version 310 es\nvoid main() {\n"
                                 "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                                 "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n";
    static const char fs_main[] = "out vec4 o;\n"
                                  "void main() { o = dv_pixel(fetch(uvec2(gl_FragCoord.xy)), work, mode); }\n";
    char cs_main[512];
    snprintf(cs_main, sizeof(cs_main),
             "layout(local_size_x = %u, local_size_y = %u) in;\n"
             "layout(rgba8, binding = 0) uniform writeonly highp image2D out_img;\n"
             "void main() {\n"
             "    uvec2 p = gl_GlobalInvocationID.xy;\n"
             "    if (p.x >= width || p.y >= height) return;\n"
             "    imageStore(out_img, ivec2(p), dv_pixel(fetch(p), work, mode));\n}\n",
             WGX, WGY);
    const char *vsp[] = {vs_src}, *fsp[] = {gl_head, core_glsl, fs_main}, *csp[] = {gl_head, core_glsl, cs_main};
    struct {
        const char *name;
        GLuint prog;
        int comp;
    } vs[2] = {{"gl-frag tex", 0, 0}, {"gl-comp tex>img", 0, 1}};
    uint32_t *out = malloc((size_t)W * H * 4);
    for (int v = 0; v < 2; v++) {
        if (!wanted(vs[v].name))
            continue;
        double tc = now_ms();
        vs[v].prog = vs[v].comp ? program(shader(GL_COMPUTE_SHADER, csp, 3), 0)
                                : program(shader(GL_VERTEX_SHADER, vsp, 1), shader(GL_FRAGMENT_SHADER, fsp, 3));
        GLuint p = vs[v].prog;
        glUseProgram(p);
        tc = now_ms() - tc;
        if (tc > 50)
            printf("%-16s (program compile %.0f ms)\n", vs[v].name, tc);
        glUniform1i(glGetUniformLocation(p, "luma_tex"), 0);
        glUniform1i(glGetUniformLocation(p, "chroma_tex"), 1);
        glUniform1ui(glGetUniformLocation(p, "width"), W);
        glUniform1ui(glGetUniformLocation(p, "height"), H);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex[0]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex[1]);
        if (vs[v].comp)
            glBindImageTexture(0, tex[2], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glViewport(0, 0, W, H);
        for (unsigned wi = 0; wi < NWORK; wi++)
            for (unsigned mode = 0; mode < 2; mode++) {
                glUniform1ui(glGetUniformLocation(p, "work"), WORKS[wi]);
                glUniform1ui(glGetUniformLocation(p, "mode"), mode);
                double t[2];
                for (int rep = 0; rep < 2; rep++) {
                    unsigned iters = rep ? ITERS : 1;
                    glFinish();
                    double t0 = now_ms();
                    for (unsigned k = 0; k < iters; k++) {
                        if (vs[v].comp) {
                            glDispatchCompute((W + WGX - 1) / WGX, (H + WGY - 1) / WGY, 1);
                            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
                        } else {
                            glDrawArrays(GL_TRIANGLES, 0, 3);
                        }
                    }
                    glFinish();
                    t[rep] = (now_ms() - t0) / iters;
                }
                memset(out, 0, (size_t)W * H * 4);
                glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out);
                GLenum e = glGetError();
                if (e)
                    fprintf(stderr, "vk-micro: GL error 0x%x\n", e);
                report(vs[v].name, WORKS[wi], mode, t[1], hash32(out, (size_t)W * H));
            }
    }
    free(out);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%ux%u", &W, &H);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            ITERS = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wg") && i + 1 < argc)
            sscanf(argv[++i], "%ux%u", &WGX, &WGY);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc)
            ONLY = argv[++i];
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) {
            NWORK = 0;
            for (char *s = strtok(argv[++i], ","); s && NWORK < 8; s = strtok(NULL, ","))
                WORKS[NWORK++] = (unsigned)atoi(s);
        } else {
            fprintf(stderr, "usage: vk-micro [--size WxH] [--iters N] [--work 0,4,16] [--wg XxY] [--only SUBSTR]\n");
            return 2;
        }
    }
    W &= ~1u;
    H &= ~1u;
    g_luma = malloc((size_t)W * H * 2);
    g_chroma = malloc((size_t)(W / 2) * (H / 2) * 4);
    uint32_t s = 12345;
    for (size_t i = 0; i < (size_t)W * H; i++)
        g_luma[i] = (uint16_t)(((s = s * 1664525u + 1013904223u) >> 16) & 0xffc0u);
    for (size_t i = 0; i < (size_t)(W / 2) * (H / 2); i++) {
        uint32_t a = ((s = s * 1664525u + 1013904223u) >> 16) & 0xffc0u;
        uint32_t b = ((s = s * 1664525u + 1013904223u) >> 16) & 0xffc0u;
        g_chroma[i] = a | b << 16;
    }
    printf("vk-micro: %ux%u, %u passes per timing, compute workgroup %ux%u\n", W, H, ITERS, WGX, WGY);
    printf("%-16s %-7s %-5s %11s  %14s  %s\n", "variant", "work", "maths", "per pass", "throughput",
           "output checksum");
    vulkan();
    gles();
    return 0;
}
