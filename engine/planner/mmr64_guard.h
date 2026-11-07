#ifndef DV_MMR64_GUARD_H
#define DV_MMR64_GUARD_H
#include <stdint.h>
#include <limits.h>
/* Exact signed conversions avoid implementation-defined unsigned narrowing. */
static inline int64_t dv_mmr_s32(uint32_t value)
{return value<UINT32_C(0x80000000)?(int64_t)value:(int64_t)value-INT64_C(4294967296);}
static inline int64_t dv_mmr_s64(uint32_t low,uint32_t high)
{uint64_t value=(uint64_t)low|((uint64_t)high<<32);
 return value<=INT64_MAX?(int64_t)value:-INT64_C(1)-(int64_t)(UINT64_MAX-value);}
/* Prove EVERY product and EVERY partial sum before launching signed64 math.
 * Original features are nonnegative and monotone after clipping, including
 * exact floor(product/2^20) at each power/triple step. All bound arithmetic is
 * unsigned64 for <=40-bit feature products and signed128 for coefficient sums.
 * No negative signed shift, abs(INT_MIN), unchecked signed overflow or relaxed
 * truncation is used. The bound is conservative; valid larger metadata falls
 * back to original96/128. Unused prepared words do not affect eligibility. */
static inline int dv_mmr64_eligible(const uint32_t p[96],uint64_t *magnitude_bound)
{
 if(!p||p[0]<1||p[0]>3||p[1]<15||p[1]>32)return 0;
 uint64_t s[3];for(unsigned k=0;k<3;++k){if(p[2+2*k]>p[3+2*k]||p[3+2*k]>1023)return 0;s[k]=p[3+2*k];}
 uint32_t sign=(p[9]&UINT32_C(0x80000000))?UINT32_MAX:0;
 if((p[8]&UINT32_C(0xfffff))||p[10]!=sign||p[11]!=sign)return 0;
 int64_t constant=dv_mmr_s64(p[8],p[9]);__int128 bound=constant;
 if(bound<0)bound=-bound;
 uint64_t first[7]={s[0]<<10,s[1]<<10,s[2]<<10,s[0]*s[1],s[0]*s[2],s[1]*s[2],(s[0]*s[1]*(s[2]<<10))>>20};
 for(unsigned k=0;k<7;++k){uint64_t feature=first[k];for(unsigned order=0;order<p[0];++order){
   const uint32_t *coefficient=p+12+4*(7*order+k);
   sign=(coefficient[0]&UINT32_C(0x80000000))?UINT32_MAX:0;
   if(coefficient[1]!=sign||coefficient[2]!=sign||coefficient[3]!=sign)return 0;
   int64_t signed_coefficient=dv_mmr_s32(coefficient[0]);
   uint64_t magnitude=(uint64_t)(signed_coefficient<0?-signed_coefficient:signed_coefficient);
   bound+=(__int128)magnitude*feature;
   feature=(first[k]*feature)>>20;
 }}
 if(magnitude_bound)*magnitude_bound=(uint64_t)bound;
 return bound<=INT64_MAX;
}
#endif
