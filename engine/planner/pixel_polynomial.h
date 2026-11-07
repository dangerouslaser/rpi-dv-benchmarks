#ifndef DV_PIXEL_POLYNOMIAL_H
#define DV_PIXEL_POLYNOMIAL_H
#include "intel_composer_config.h"
/* Already-aligned component codes; no chroma resampling or colour conversion. */
int dv_reference_polynomial(const dv_intel_composer_config *,unsigned,uint16_t,
                           uint16_t,int32_t *,int32_t *,uint16_t *);
/* Single-layer polynomial mapping. Failure leaves all table entries unchanged. */
int dv_prepare_polynomial_tables(const dv_intel_composer_config *,uint16_t [3][1024]);
/* FEL tables retain Q16 mapping and signed residual separately. Quantize only
 * after adding them, never through the single-layer 12-bit table. */
int dv_prepare_polynomial_component(const dv_intel_composer_config *,unsigned,
                                    uint16_t [1024],int32_t [1024]);
#endif
