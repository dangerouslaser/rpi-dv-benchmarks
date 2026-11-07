/* The Pi FEL engine: Dolby Vision dual-layer (and single-layer MMR / polynomial
 * chroma) reconstruction into the TV-led tunnel frame, as two Vulkan
 * fragment passes, built for V3D (docs/pi5/V3DV.md, docs/pi5/FEL-ENGINE.md).
 * The CPU half is yblod's own planner (plan.c), so the tables, metadata
 * packets and pack constants are yblod's; the passes compute what yblod's
 * bilinear FEL route computes, bit for bit except the chroma MMR in float mode.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <libavutil/frame.h>
#include <stdint.h>

struct pi_engine;

/* MMR: float; exact in 32-bit pieces; exact in emulated 64-bit; exact with
 * the 64-bit sum's low word in 32-bit integers and its high word from a
 * float sum (the default; frames whose magnitudes don't allow it: 64-bit). */
enum pi_mmr_mode { PI_MMR_FLOAT = 0, PI_MMR_EXACT32 = 1, PI_MMR_EXACT64 = 2, PI_MMR_HYBRID = 3, PI_MMR_BOUNDED = 4 };

/* device: substring of the Vulkan device name, or NULL (the first GPU). */
struct pi_engine *pi_create(const char *device, enum pi_mmr_mode mode, int cb1_contract);
/* Output slot `slot` (0..2): a linear XRGB8888 dmabuf, stride bytes a row,
 * at least stride * (h + 1) bytes (the odd rows are a second image). */
int pi_set_output(struct pi_engine *e, unsigned slot, int fd, uint32_t stride);
/* Plans and submits picture bl (+ el) with its AV_FRAME_DATA_DOVI_METADATA
 * into output `slot`. 0 OK; <0 error; >0 the planner's refusal (a dv_status). */
int pi_submit(struct pi_engine *e, const AVFrame *bl, const AVFrame *el, const void *rpu, size_t rpu_size,
              int chroma_top_left, int64_t pts_us, int first, unsigned slot);
/* 1 when slot's last submission is done (or there is none), never blocks. */
int pi_done(struct pi_engine *e, unsigned slot);
/* Waits for slot's last submission. 0 OK. */
int pi_wait(struct pi_engine *e, unsigned slot);
/* GPU time of slot's last frame per pass (ms; 0 if not measured), and the
 * planner's CPU time. */
void pi_times(struct pi_engine *e, unsigned slot, double pass_ms[3], double *plan_ms);
const char *pi_describe(struct pi_engine *e);
void pi_destroy(struct pi_engine *e);
