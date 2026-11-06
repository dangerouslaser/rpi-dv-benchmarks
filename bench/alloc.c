/* See alloc.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "alloc.h"
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/udmabuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *kind;

static int udmabuf(size_t size)
{
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0)
        return -1;
    int mem = memfd_create("dvbench", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    int fd = -1;
    if (mem >= 0 && !ftruncate(mem, (off_t)size) && fcntl(mem, F_ADD_SEALS, F_SEAL_SHRINK) >= 0) {
        struct udmabuf_create c = {.memfd = (uint32_t)mem, .flags = UDMABUF_FLAGS_CLOEXEC, .size = size};
        fd = ioctl(dev, UDMABUF_CREATE, &c);
    }
    if (mem >= 0)
        close(mem);
    close(dev);
    return fd;
}

static int heap(const char *name, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/dma_heap/%s", name);
    int dev = open(path, O_RDWR | O_CLOEXEC);
    if (dev < 0)
        return -1;
    struct dma_heap_allocation_data a = {.len = size, .fd_flags = O_RDWR | O_CLOEXEC};
    int r = ioctl(dev, DMA_HEAP_IOCTL_ALLOC, &a);
    close(dev);
    return r < 0 ? -1 : (int)a.fd;
}

static int allocate(size_t size)
{
    const char *want = getenv("DVBENCH_ALLOC");
    if (kind) {
        if (!strcmp(kind, "udmabuf"))
            return udmabuf(size);
        return heap(!strcmp(kind, "cma") ? "linux,cma" : "system", size);
    }
    int fd;
    if ((!want || !strcmp(want, "udmabuf")) && (fd = udmabuf(size)) >= 0) {
        kind = "udmabuf";
        return fd;
    }
    if ((!want || !strcmp(want, "cma")) && (fd = heap("linux,cma", size)) >= 0) {
        kind = "cma";
        return fd;
    }
    if ((!want || !strcmp(want, "system")) && (fd = heap("system", size)) >= 0) {
        kind = "system";
        return fd;
    }
    fprintf(stderr, "alloc: no udmabuf or DMA heap%s%s\n", want ? " for DVBENCH_ALLOC=" : "", want ? want : "");
    return -1;
}

int dv_dmabuf_alloc(struct dv_dmabuf *b, size_t size)
{
    size = (size + 4095) & ~(size_t)4095;
    memset(b, 0, sizeof(*b));
    b->fd = allocate(size);
    if (b->fd < 0)
        return -1;
    b->size = size;
    b->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->map == MAP_FAILED) {
        perror("alloc: mmap");
        close(b->fd);
        b->fd = -1;
        return -1;
    }
    return 0;
}

void dv_dmabuf_free(struct dv_dmabuf *b)
{
    if (b->map && b->map != MAP_FAILED)
        munmap(b->map, b->size);
    if (b->fd >= 0)
        close(b->fd);
    memset(b, 0, sizeof(*b));
    b->fd = -1;
}

void dv_dmabuf_cpu(const struct dv_dmabuf *b, int begin, int write)
{
    struct dma_buf_sync s = {.flags = (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                                      (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ)};
    ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &s);
}

const char *dv_dmabuf_kind(void) { return kind ? kind : "none yet"; }
