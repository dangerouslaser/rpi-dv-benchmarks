/* CPU-mappable dmabufs for the bench's own buffers (software pictures,
 * engine outputs): udmabuf where the kernel has it, else a DMA heap
 * (Raspberry Pi OS: /dev/dma_heap/linux,cma and system). DVBENCH_ALLOC=
 * udmabuf|cma|system picks one.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct dv_dmabuf {
    int fd;
    size_t size;
    uint8_t *map;
};

/* size rounded up to whole pages; 0 or -1. */
int dv_dmabuf_alloc(struct dv_dmabuf *b, size_t size);
void dv_dmabuf_free(struct dv_dmabuf *b);
/* Bracket CPU access (DMA_BUF_IOCTL_SYNC). */
void dv_dmabuf_cpu(const struct dv_dmabuf *b, int begin, int write);
const char *dv_dmabuf_kind(void);
