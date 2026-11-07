/* big-fella's Dolby Vision source-tunnel engine on Vulkan compute
 * (docs/vulkan-engine.md).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "dvvk_plan.h"   /* dvvk_plan_in */
#ifdef __cplusplus
extern "C" {
#endif

/* The library to dlopen (also its soname: build.rs reads it from here). A
 * layout change of any struct below is a new soname and DVVK_ABI_VERSION. */
#define DVVK_LIBRARY "libdv_engine_vk.so.1"
/* 1: libdv_engine_vk.so.0 (until 0.1.0-8); 2: dvvk_plan_in.cb1_contract. */
#define DVVK_ABI_VERSION 2u
/* The ABI the loaded library was built with: a caller compares it with its
 * own DVVK_ABI_VERSION before using anything else. */
uint32_t dvvk_abi_version(void);

typedef struct dvvk_engine dvvk_engine;
typedef struct {
    dvvk_plan_in plan;            /* geometry, scaler, packet id, refresh, overlay */
    uint32_t bl_surface, el_surface;  /* VASurfaceID; el 0xffffffff = none */
    const void *metadata; size_t metadata_size;   /* ffmpeg-yblod AVDOVIMetadata */
    uint32_t output;              /* slot */
    uint32_t flags;               /* DVVK_FRAME_AUTO_OVERLAY: the overlay (and the
                                   * metadata's overlay edge) from the engine's
                                   * sources, set_subtitles / set_overlay */
    uint64_t release_wait;        /* explicit sync: write the output only once the
                                   * release timeline reaches this (0: no wait) */
} dvvk_frame;
#define DVVK_FRAME_AUTO_OVERLAY 1u
/* Desktop mode: the frame gets static graphics display management (L1 from
 * PQ(0.005) to the film's peak, a fixed average, no trims) instead of the
 * film's per-scene metadata: the film is a small part of a frame the desktop
 * fills (docs/research/media-following-colour.md). */
#define DVVK_FRAME_DESKTOP 2u

typedef struct {
    int32_t x, y;                 /* destination in the 3840x2160 canvas */
    uint32_t w, h, dw, dh;        /* bitmap size; destination size (BGRA may scale) */
    uint32_t stride;
    uint32_t format;              /* 1 libass A8 with color, 2 BGRA premultiplied */
    uint32_t color;               /* libass 0xRRGGBBAA, AA = transparency */
    const void *data;
} dvvk_sub_part;

typedef struct {
    uint32_t width, height, format;   /* DRM fourcc: AR24, XR24, AB24, XB24 */
    uint64_t modifier;
    uint32_t num_planes;
    int fd[4];
    uint32_t offset[4], stride[4];
} dvvk_dmabuf;

/* Overlay sources for frames with DVVK_FRAME_AUTO_OVERLAY. */
int dvvk_set_subtitles(dvvk_engine *, const dvvk_sub_part *, uint32_t num);
int dvvk_set_overlay(dvvk_engine *, const dvvk_dmabuf *, uint64_t id);   /* NULL: none */
/* With flags: DVVK_OVERLAY_FLIP_Y, rows bottom first (a GL framebuffer's
 * texture: Kodi's GUI, read in place); its damage counts as everywhere. Other
 * flags: -1. */
#define DVVK_OVERLAY_FLIP_Y 1u
int dvvk_set_overlay_ex(dvvk_engine *, const dvvk_dmabuf *, uint64_t id, uint32_t flags);
/* Every frame that reads the overlay buffer leaves its fence on it as an
 * implicit read fence: a renderer that honours implicit sync may draw the
 * next contents into it at once (no copy, no CPU wait). */
/* The next dvvk_set_overlay's buffer differs from the previous one only in
 * these count rectangles (x, y, w, h each; 0: the same picture). Without it a
 * new buffer counts as changed everywhere. The engine then rewrites only what
 * changed in each output buffer (DVVK_DAMAGE=0: everything, every frame). */
int dvvk_set_overlay_damage(dvvk_engine *, const uint32_t *rects, uint32_t count);
/* 1 once a fence could not be attached as an implicit fence (older kernel):
 * wait for each ticket before committing or drawing into the overlay. */
int dvvk_implicit_failed(dvvk_engine *);
uint64_t dvvk_overlay_ticket(dvvk_engine *, uint64_t id);
int dvvk_create(int drm_fd, void *va_display, dvvk_engine **out);
int dvvk_set_output(dvvk_engine *, uint32_t slot, int dmabuf_fd, uint32_t stride);
/* 0: the picture is in the output; > 0 never; < 0: a dv_status (-2: not this
 * engine's frame, show an ordinary picture; -8: engine failure). */
int dvvk_render_sync(dvvk_engine *, const dvvk_frame *, uint32_t *error_flags);
/* Phase 1: the overlay picture for frames with plan.overlay set (RGBA8
 * premultiplied sRGB, 3840x2160, rows top first), copied in. Error flags
 * bit 8: an overlay colour the source can't represent. */
void dvvk_set_overlay_rgba(dvvk_engine *, const void *rgba, size_t stride);
/* Asynchronous: plan and submit without waiting; 0 and *ticket, or a
 * dv_status (*ticket is zero on failure). On success the output dmabuf carries the frame's fence as an
 * implicit write fence (commit it at once). The decoded surfaces must stay
 * alive until the ticket is done. A -8 recording/submission/export failure
 * requires destroying/recreating the engine; reset is not error recovery.
 * Export failure attempts a bounded drain, which can itself fail. On -8 keep
 * ALL inputs (including this frame with no returned ticket) until dvvk_drain
 * returns 1 and destruction succeeds. No device-loss drain is promised. */
int dvvk_submit(dvvk_engine *, const dvvk_frame *, uint64_t *ticket);
/* The CM4 / L2 result of the last frame planned (dvvk_plan_in.dm_options):
 * DVVK_CM4_STATUS_* | L2 blocks removed << 8, UINT32_MAX for NULL. Added in
 * 0.1.0-18 without an ABI change: callers look it up with dlsym. */
uint32_t dvvk_dm_status(dvvk_engine *);
/* Never blocks: *done when finished; *flags (1 range, 2 overflow, 0x100
 * overlay colour) once done. An error is not completion: do not read the
 * output or release its inputs on a failed poll. */
int dvvk_poll(dvvk_engine *, uint64_t ticket, int *done, uint32_t *flags);
/* 1 done, 0 timeout, < 0 failure. Only 1 authorizes reading the output or
 * releasing inputs. A failed reset retirement keeps imports and poisons submit. */
int dvvk_wait(dvvk_engine *, uint64_t ticket, uint64_t timeout_ns);
/* linux-drm-syncobj-v1: DRM syncobj fds of the frames' timeline (acquire
 * point = the frame's ticket) and of a release timeline for the compositor's
 * release points. The caller owns the fds. */
int dvvk_timelines(dvvk_engine *, int *acquire_syncobj_fd, int *release_syncobj_fd);
void dvvk_decode_waits(dvvk_engine *, uint64_t *fence, uint64_t *vasync);
/* 1 complete, 0 timeout, <0 failure; includes submitted work without a returned
 * ticket. Failure poisons submit. This retains all imports on every result. */
int dvvk_drain(dvvk_engine *, uint64_t timeout_ns);
/* 1 reset, 0 timeout, <0 failure. Failed reset retains imports. Poisoned
 * engines cannot reset successfully; destroy/recreate only after retirement. */
int dvvk_reset_checked(dvvk_engine *, uint64_t timeout_ns);
void dvvk_reset(dvvk_engine *);
/* Decoders without VAAPI (the Raspberry Pi 5's V4L2 request HEVC decoder):
 * create the engine with va_display NULL and register each decoded picture
 * under the id its frames then name in bl_surface / el_surface. Registered
 * pictures are re-checked every frame (re-register an id whenever its buffer
 * changes; the fds are dup'd). Plane 0 luma, plane 1 chroma; formats:
 *   DRM_FORMAT_P010 / NV12, LINEAR or a modifier Vulkan can import;
 *   DRM_FORMAT_P030 with DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(h): the
 *   column height h for both planes on one object, or h = 0 with each plane's
 *   column height in its pitch (one object each, V4L2 *_COL128M), as
 *   FFmpeg's DRM PRIME descriptor gives them. Unpacked by the engine on the
 *   GPU into P010-identical planes each frame.
 * The caller hands over finished pictures (no VA sync without VA). */
typedef struct {
    uint32_t drm_format, width, height;   /* allocated size */
    uint32_t num_objects;                 /* 1, or 2 (one per plane) */
    int fd[2];
    uint32_t size[2];
    uint64_t modifier;
    uint32_t object[2], offset[2], pitch[2];  /* per plane */
} dvvk_surface_dmabuf;
int dvvk_set_surface_dmabuf(dvvk_engine *, uint32_t id, const dvvk_surface_dmabuf *);
/* Decoded surfaces are imported once and then known by their VASurfaceID
 * until a reset: reset whenever the decoder's surface pool changes (a new
 * AVHWFramesContext may reuse ids for other allocations), and keep the old
 * pool alive until then. */
/* Legacy void destruction waits for device idle. Idle failure terminates the
 * process without freeing resources; returning would imply unsafe completion.
 * Prefer a finite dvvk_drain before destruction. Vulkan device_wait_idle itself
 * has no timeout: no guarantee is made for a driver stuck inside that call. */
void dvvk_destroy(dvvk_engine *);
#if UINTPTR_MAX == UINT64_MAX
/* The layouts (LP64), as src/abi.rs asserts them for the Rust mirrors. */
DVVK_STATIC_ASSERT(sizeof(dvvk_frame) == 200, "dvvk_frame layout");
DVVK_STATIC_ASSERT(sizeof(dvvk_surface_dmabuf) == 64, "dvvk_surface_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_surface_dmabuf, fd) == 16, "dvvk_surface_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_surface_dmabuf, modifier) == 32, "dvvk_surface_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_surface_dmabuf, pitch) == 56, "dvvk_surface_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_frame, bl_surface) == 160, "dvvk_frame layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_frame, metadata) == 168, "dvvk_frame layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_frame, metadata_size) == 176, "dvvk_frame layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_frame, output) == 184, "dvvk_frame layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_frame, flags) == 188, "dvvk_frame layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_frame, release_wait) == 192, "dvvk_frame layout");
DVVK_STATIC_ASSERT(sizeof(dvvk_sub_part) == 48, "dvvk_sub_part layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_sub_part, stride) == 24, "dvvk_sub_part layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_sub_part, color) == 32, "dvvk_sub_part layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_sub_part, data) == 40, "dvvk_sub_part layout");
DVVK_STATIC_ASSERT(sizeof(dvvk_dmabuf) == 80, "dvvk_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_dmabuf, modifier) == 16, "dvvk_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_dmabuf, num_planes) == 24, "dvvk_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_dmabuf, fd) == 28, "dvvk_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_dmabuf, offset) == 44, "dvvk_dmabuf layout");
DVVK_STATIC_ASSERT(offsetof(dvvk_dmabuf, stride) == 60, "dvvk_dmabuf layout");
#endif
#ifdef __cplusplus
}
#endif
