/* Create the Vulkan engine and destroy it: every pipeline it builds at
 * startup gets compiled. With Mesa's V3D drm-shim preloaded and
 * V3D_DEBUG=shaderdb that prints the V3D compiler's statistics for each, no
 * Pi needed (scripts/shader-stats.sh).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "dv_engine_vk.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    void *h = dlopen(DVVK_LIBRARY, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    int (*create)(int, void *, dvvk_engine **) = dlsym(h, "dvvk_create");
    void (*destroy)(dvvk_engine *) = dlsym(h, "dvvk_destroy");
    int fd = open(node, O_RDWR | O_CLOEXEC);
    dvvk_engine *e = NULL;
    if (!create || create(fd, NULL, &e)) {
        fprintf(stderr, "vk-compile: engine didn't start on %s (fd %d)\n", node, fd);
        return 1;
    }
    printf("vk-compile: engine created\n");
    destroy(e);
    return 0;
}
