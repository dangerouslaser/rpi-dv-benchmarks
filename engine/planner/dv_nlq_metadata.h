#ifndef DV_NLQ_METADATA_H
#define DV_NLQ_METADATA_H
#include <libavutil/dovi_meta.h>
#include <stdint.h>
/* Call only with bounds-checked, copied metadata structures. Canonical MEL
 * can leave disable_residual clear while its NLQ produces exactly zero. */
static inline int dv_metadata_no_residual(const AVDOVIRpuDataHeader *h,
                                         const AVDOVIDataMapping *m)
{
    if(h->disable_residual_flag==1)return 1;
    if(h->disable_residual_flag||h->coef_data_type||h->coef_log2_denom>32||
       h->bl_bit_depth!=10||h->el_bit_depth!=10||h->vdr_bit_depth!=12||
       m->nlq_method_idc!=AV_DOVI_NLQ_LINEAR_DZ)return 0;
    uint64_t maximum=UINT64_C(1)<<h->coef_log2_denom;
    for(unsigned c=0;c<3;++c){const AVDOVINLQParams *n=&m->nlq[c];
        if(n->nlq_offset||n->vdr_in_max!=maximum||n->linear_deadzone_slope||
           n->linear_deadzone_threshold)return 0;}
    return 1;
}
#endif
