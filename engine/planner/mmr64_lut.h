#ifndef DV_MMR64_LUT_H
#define DV_MMR64_LUT_H
#include "mmr64_guard.h"
#include <string.h>
enum {DV_MMR64_LUT_FEATURES=3,DV_MMR64_LUT_SAMPLES=1024};
typedef struct {
 uint32_t key[96];
 int64_t table[DV_MMR64_LUT_FEATURES][DV_MMR64_LUT_SAMPLES];
 int valid;
} dv_mmr64_lut_cache;
/* Only the proven signed64 domain is supported. Each entry is a subset of
 * original terms, so the absolute-partial-sum bound also proves this sum fits.
 * Preserve the original floor(first*previous/2^20) at EVERY power. */
static inline int dv_mmr64_build_lut(const uint32_t p[96],int64_t table[3][1024])
{
 if(!table||!dv_mmr64_eligible(p,NULL))return 0;
 for(unsigned k=0;k<3;++k)for(unsigned input=0;input<1024;++input){
  uint64_t sample=input<p[2+2*k]?p[2+2*k]:(input>p[3+2*k]?p[3+2*k]:input);
  uint64_t first=sample<<10,feature=first;__int128 value=0;
  for(unsigned order=0;order<p[0];++order){
   value+=(__int128)dv_mmr_s32(p[12+4*(7*order+k)])*feature;
   feature=(first*feature)>>20;
  }
  table[k][input]=(int64_t)value;
 }
 return 1;
}
static inline int dv_mmr64_lut_matches(const dv_mmr64_lut_cache *cache,const uint32_t p[96])
{return cache&&p&&cache->valid&&!memcmp(cache->key,p,sizeof(cache->key));}
/* Backend commits this key ONLY AFTER a successful blocking device upload.
 * All96 prepared words, including unused words, participate in invalidation. */
static inline void dv_mmr64_lut_commit(dv_mmr64_lut_cache *cache,const uint32_t p[96])
{memcpy(cache->key,p,sizeof(cache->key));cache->valid=1;}
#endif
