#ifndef DV_MMR_PREPARE_H
#define DV_MMR_PREPARE_H
#include "dv_engine.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 96 scalar words: order,denom,three pivot pairs,constant uint4,21 uint4
 * sign-extended coefficients. Explicit words avoid host/shader struct ABI. */
dv_status dv_mmr_prepare(const dv_intel_composer_config *,unsigned,uint32_t[96],int32_t[1024]);
#ifdef __cplusplus
}
#endif
#endif
