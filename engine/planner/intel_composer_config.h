/* Private, hardware-independent composer contract. No Amlogic addresses,
 * HDMI packing, display mapping, or guessed enhancement-scaling filter.
 */
#ifndef DV_INTEL_COMPOSER_CONFIG_H
#define DV_INTEL_COMPOSER_CONFIG_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t integer;
    uint32_t fraction;
} dv_exact_coefficient;

typedef struct {
    uint32_t mapping;
    uint32_t pivot_count;
    uint32_t pivots[9];
    uint32_t polynomial_order[8];
    dv_exact_coefficient polynomial[8][3];
    uint32_t mmr_order;
    dv_exact_coefficient mmr_constant;
    dv_exact_coefficient mmr[3][7];
    uint32_t nlq_offset;
    dv_exact_coefficient nlq[3]; /* slope, input maximum, threshold */
} dv_component_config;

typedef struct {
    uint32_t abi_version;
    uint32_t reconstruction_depth;
    uint32_t base_depth;
    uint32_t enhancement_depth;
    uint32_t coefficient_log2_denominator;
    uint32_t residual_enabled;
    uint32_t el_spatial_resampling_flag; /* Not a filter selection. */
    uint32_t base_full_range;
    dv_component_config component[3];
} dv_intel_composer_config;

#endif
