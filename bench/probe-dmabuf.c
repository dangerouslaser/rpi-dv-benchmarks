/* Can this machine's EGL/GLES and Vulkan import a udmabuf dmabuf? (The bench's
 * software frame source needs both.) Prints one line per check. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

static int make_udmabuf(size_t size)
{
    int mem = memfd_create("probe", MFD_ALLOW_SEALING);
    if (mem < 0 || ftruncate(mem, (off_t)size) || fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK) < 0)
        return -1;
    int dev = open("/dev/udmabuf", O_RDWR);
    if (dev < 0)
        return -1;
    struct udmabuf_create c = {.memfd = (uint32_t)mem, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size};
    int fd = ioctl(dev, UDMABUF_CREATE, &c);
    close(dev);
    close(mem);
    return fd;
}

int main(void)
{
    const int w = 256, h = 64;
    int fd = make_udmabuf((size_t)w * h * 2);
    printf("udmabuf: %s\n", fd >= 0 ? "ok" : "FAILED");
    if (fd < 0)
        return 1;

    EGLDisplay dpy = ((PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT"))(
        EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    int egl = dpy && eglInitialize(dpy, NULL, NULL) && eglBindAPI(EGL_OPENGL_ES_API);
    const char *ext = egl ? eglQueryString(dpy, EGL_EXTENSIONS) : "";
    printf("egl surfaceless: %s, dma_buf_import %s, modifiers %s\n", egl ? "ok" : "FAILED",
           strstr(ext, "EGL_EXT_image_dma_buf_import") ? "yes" : "no",
           strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") ? "yes" : "no");
    if (egl) {
        const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        printf("gles: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        const EGLint ia[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R16,
                             EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                             EGL_DMA_BUF_PLANE0_PITCH_EXT, w * 2, EGL_NONE};
        EGLImageKHR img = ((PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR"))(
            dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
        printf("egl R16 dmabuf image: %s (0x%x)\n", img ? "ok" : "FAILED", img ? 0 : eglGetError());
    }

    VkInstance inst;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("vulkan: FAILED\n");
        return 1;
    }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1,
                                 .pQueuePriorities = &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &q, .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("vulkan device: FAILED\n");
        return 1;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb,
                              .size = (VkDeviceSize)w * h * 2, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    PFN_vkGetMemoryFdPropertiesKHR props_fn = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult pr = props_fn(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup(fd), &props);
    uint32_t bits = req.memoryTypeBits & (pr ? ~0u : props.memoryTypeBits), type = 0;
    while (type < 32 && !(bits & (1u << type)))
        type++;
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd)};
    /* At the dmabuf's own size: drivers pad requirements (v3dv: +64 bytes),
     * and an import larger than the dmabuf fails (the engine does the same). */
    off_t fd_size = lseek(fd, 0, SEEK_END);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                .allocationSize = fd_size > 0 ? (VkDeviceSize)fd_size : req.size,
                                .memoryTypeIndex = type};
    printf("vulkan buffer requirement %llu bytes, dmabuf %lld bytes\n", (unsigned long long)req.size,
           (long long)fd_size);
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &mai, NULL, &mem);
    printf("vulkan dmabuf buffer import: %s (%d)\n", r == VK_SUCCESS ? "ok" : "FAILED", r);
    return 0;
}
