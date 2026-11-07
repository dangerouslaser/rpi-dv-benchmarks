/* Shared between the engine's C files.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(h): vendor 0x07 in bits 56..63,
 * SAND128 (4) in bits 0..7, the column height in bits 8..55. */
enum { PI_MOD_VENDOR_BROADCOM = 0x07, PI_MOD_SAND128 = 4 };
static inline uint32_t pi_sand_column_height(uint64_t m) { return (uint32_t)((m >> 8) & 0xffffffffu); }
