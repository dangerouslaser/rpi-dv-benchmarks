#include "single_reshape.h"
#include "dv_nlq_metadata.h"
#include <libavutil/dovi_meta.h>
#include <string.h>
static int contains(size_t offset,size_t length,size_t total)
{return offset>=sizeof(AVDOVIMetadata)&&offset<=total&&length<=total-offset;}
int dv_single_reshape_from_ffmpeg(const void *data,size_t bytes,dv_single_reshape *out)
{
    if(!data||!out||bytes<sizeof(AVDOVIMetadata))return -1;
    AVDOVIMetadata metadata;memcpy(&metadata,data,sizeof(metadata));
    if(!contains(metadata.header_offset,sizeof(AVDOVIRpuDataHeader),bytes)||
       !contains(metadata.mapping_offset,sizeof(AVDOVIDataMapping),bytes))return -1;
    AVDOVIRpuDataHeader h;AVDOVIDataMapping mapping;const unsigned char *raw=data;
    memcpy(&h,raw+metadata.header_offset,sizeof(h));memcpy(&mapping,raw+metadata.mapping_offset,sizeof(mapping));
    if(h.coef_data_type||h.coef_log2_denom>32||h.bl_bit_depth<8||h.bl_bit_depth>12||
       h.vdr_bit_depth!=12||!dv_metadata_no_residual(&h,&mapping)||h.ext_mapping_idc_0_4||h.ext_mapping_idc_5_7||
       mapping.mapping_color_space||mapping.mapping_chroma_format_idc)return -1;
    dv_single_reshape result={0};result.base_depth=h.bl_bit_depth;result.denominator=h.coef_log2_denom;
    unsigned maximum=(1u<<result.base_depth)-1;
    for(unsigned c=0;c<3;++c){const AVDOVIReshapingCurve *v=&mapping.curves[c];dv_reshape_curve *dst=&result.curve[c];
        if(v->num_pivots<2||v->num_pivots>9)return -1;
        dst->pivot_count=v->num_pivots;
        for(unsigned p=0;p<v->num_pivots;++p){
            if(v->pivots[p]>maximum||(p&&v->pivots[p]<=v->pivots[p-1]))return -1;
            dst->pivots[p]=v->pivots[p];}
        for(unsigned p=0;p+1<v->num_pivots;++p){dv_reshape_piece *piece=&dst->piece[p];
            switch(v->mapping_idc[p]){
            case AV_DOVI_MAPPING_POLYNOMIAL:
                if(v->poly_order[p]<1||v->poly_order[p]>2)return -1;
                piece->order=v->poly_order[p];piece->constant=v->poly_coef[p][0];
                for(unsigned k=0;k<piece->order;++k)piece->coefficient[0][k]=v->poly_coef[p][k+1];
                break;
            case AV_DOVI_MAPPING_MMR:
                if(v->mmr_order[p]<1||v->mmr_order[p]>3)return -1;
                piece->method=1;piece->order=v->mmr_order[p];piece->constant=v->mmr_constant[p];
                for(unsigned order=0;order<piece->order;++order)
                    for(unsigned k=0;k<7;++k)piece->coefficient[order][k]=v->mmr_coef[p][order][k];
                break;
            default:return -1;
            }
        }
    }
    *out=result;return 0;
}
