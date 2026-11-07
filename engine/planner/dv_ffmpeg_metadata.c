#include "dv_ffmpeg_metadata.h"
#include "dv_cm4.h"
#include "dv_nlq_metadata.h"
#include <libavutil/dovi_meta.h>
#include <limits.h>
#include <string.h>

#ifdef DV_FFMPEG_RAW_EXTENSIONS
/* This feature is compiled only against the matching private FFmpeg header.
 * The public prefix still accepts unextended metadata for CM2.9. */
#define EXT_PREFIX offsetof(AVDOVIDmData,dvbridge_raw_magic)
#else
#define EXT_PREFIX sizeof(AVDOVIDmData)
#endif

static int span(size_t offset,size_t length,size_t total)
{return offset>=sizeof(AVDOVIMetadata)&&offset<=total&&length<=total-offset;}
static int coefficient(int64_t value,unsigned denominator,dv_exact_coefficient *out)
{
    int64_t unit=INT64_C(1)<<denominator;
    int64_t integer=value/unit,remainder=value%unit;
    if(remainder<0){--integer;remainder+=unit;}
    if(integer<INT32_MIN||integer>INT32_MAX)return -1;
    out->integer=(int32_t)integer;out->fraction=(uint32_t)remainder;return 0;
}
static int rational(AVRational value,int64_t scale,int64_t low,int64_t high,int64_t *out)
{
    if(value.den<=0)return -1;
    int64_t scaled=(int64_t)value.num*scale;
    if(scaled%value.den)return -1;
    scaled/=value.den;if(scaled<low||scaled>high)return -1;*out=scaled;return 0;
}
static void put16(unsigned char *p,uint32_t value)
{p[0]=(unsigned char)(value>>8);p[1]=(unsigned char)value;}
static void put32(unsigned char *p,uint32_t value)
{put16(p,value>>16);put16(p+2,value&65535u);}

static dv_status settings_from_ffmpeg(const void *data,size_t bytes,unsigned width,unsigned height,
                                 dv_identity identity,dv_frame_settings *out,
                                 unsigned char source_dm[512],size_t *source_bytes,
                                 dv_single_reshape *single,int source_transport,unsigned sample_depth)
{
    if(!data||!out||!source_dm||!source_bytes||bytes<sizeof(AVDOVIMetadata)||
       !width||!height||width>4096||height>4096||width%4||height%4)return DV_INVALID;
    const unsigned char *raw=data;AVDOVIMetadata metadata;memcpy(&metadata,data,sizeof(metadata));
    if(!span(metadata.header_offset,sizeof(AVDOVIRpuDataHeader),bytes)||
       !span(metadata.mapping_offset,sizeof(AVDOVIDataMapping),bytes)||
       !span(metadata.color_offset,sizeof(AVDOVIColorMetadata),bytes)||
       metadata.num_ext_blocks<0||metadata.num_ext_blocks>AV_DOVI_MAX_EXT_BLOCKS)return DV_INVALID;
    if(metadata.num_ext_blocks&&(metadata.ext_block_size<EXT_PREFIX||
       metadata.ext_block_size>bytes/(size_t)metadata.num_ext_blocks||
       !span(metadata.ext_block_offset,metadata.ext_block_size*(size_t)metadata.num_ext_blocks,bytes)))return DV_INVALID;
    AVDOVIRpuDataHeader header;AVDOVIDataMapping mapping;AVDOVIColorMetadata color;
    memcpy(&header,raw+metadata.header_offset,sizeof(header));
    memcpy(&mapping,raw+metadata.mapping_offset,sizeof(mapping));
    memcpy(&color,raw+metadata.color_offset,sizeof(color));
    if(header.coef_data_type!=0||header.coef_log2_denom>32||header.bl_bit_depth!=(single?sample_depth:10u)||
       (header.el_bit_depth!=10&&!(single&&sample_depth==8&&header.el_bit_depth==8))||
       header.vdr_bit_depth!=12||header.disable_residual_flag>1||
       header.el_spatial_resampling_filter_flag>1||header.bl_video_full_range_flag>1||
       header.chroma_resampling_explicit_filter_flag||header.ext_mapping_idc_0_4||header.ext_mapping_idc_5_7||
       mapping.mapping_color_space||mapping.mapping_chroma_format_idc||
       color.signal_bit_depth!=12||(color.signal_color_space!=0&&(!source_transport||color.signal_color_space!=2))||color.signal_chroma_format!=0||
       color.signal_full_range_flag!=1||color.signal_eotf!=65535||color.signal_eotf_param0||
       color.signal_eotf_param1||color.signal_eotf_param2)return DV_UNSUPPORTED;
    if(!header.disable_residual_flag&&mapping.nlq_method_idc!=AV_DOVI_NLQ_LINEAR_DZ)return DV_UNSUPPORTED;
    dv_frame_settings result={0};result.abi_version=DV_ENGINE_ABI;result.identity=identity;
    result.active[2]=width;result.active[3]=height;result.pq_policy=1;result.el_scaler=1;result.colour_cache=2;
    dv_intel_composer_config *cfg=&result.composer;cfg->abi_version=1;
    cfg->reconstruction_depth=header.vdr_bit_depth;cfg->base_depth=header.bl_bit_depth;
    cfg->enhancement_depth=header.el_bit_depth;cfg->coefficient_log2_denominator=header.coef_log2_denom;
    cfg->residual_enabled=(uint32_t)!dv_metadata_no_residual(&header,&mapping);cfg->el_spatial_resampling_flag=header.el_spatial_resampling_filter_flag;
    cfg->base_full_range=header.bl_video_full_range_flag;
    dv_single_reshape single_result;
    if(single&&dv_single_reshape_from_ffmpeg(data,bytes,&single_result))return DV_UNSUPPORTED;
    if(!single)for(unsigned c=0;c<3;++c){const AVDOVIReshapingCurve *curve=&mapping.curves[c];dv_component_config *dst=&cfg->component[c];
        if(curve->num_pivots<2||curve->num_pivots>9)return DV_INVALID;
        dst->pivot_count=curve->num_pivots;
        if(curve->mapping_idc[0]!=AV_DOVI_MAPPING_POLYNOMIAL&&curve->mapping_idc[0]!=AV_DOVI_MAPPING_MMR)return DV_UNSUPPORTED;
        dst->mapping=(uint32_t)curve->mapping_idc[0];
        for(unsigned p=0;p<curve->num_pivots;++p){dst->pivots[p]=curve->pivots[p];
            if(curve->pivots[p]>1023||(p&&curve->pivots[p]<=curve->pivots[p-1]))return DV_INVALID;}
        for(unsigned p=0;p+1u<curve->num_pivots;++p){
            if(curve->mapping_idc[p]!=curve->mapping_idc[0])return DV_UNSUPPORTED;
            if(!dst->mapping){if(curve->poly_order[p]>2)return DV_UNSUPPORTED;dst->polynomial_order[p]=curve->poly_order[p];
                for(unsigned k=0;k<=curve->poly_order[p];++k)if(coefficient(curve->poly_coef[p][k],header.coef_log2_denom,&dst->polynomial[p][k]))return DV_UNSUPPORTED;
            }else{
                if(!c||curve->mmr_order[p]<1||curve->mmr_order[p]>3)return DV_UNSUPPORTED;
                if(p&&(curve->mmr_order[p]!=curve->mmr_order[0]||curve->mmr_constant[p]!=curve->mmr_constant[0]))return DV_UNSUPPORTED;
                dst->mmr_order=curve->mmr_order[p];
                if(coefficient(curve->mmr_constant[p],header.coef_log2_denom,&dst->mmr_constant))return DV_UNSUPPORTED;
                for(unsigned order=0;order<dst->mmr_order;++order)for(unsigned k=0;k<7;++k){
                    if(p&&curve->mmr_coef[p][order][k]!=curve->mmr_coef[0][order][k])return DV_UNSUPPORTED;
                    if(coefficient(curve->mmr_coef[p][order][k],header.coef_log2_denom,&dst->mmr[order][k]))return DV_UNSUPPORTED;}
            }
        }
        dst->nlq_offset=mapping.nlq[c].nlq_offset;
        uint64_t nlq[3]={mapping.nlq[c].linear_deadzone_slope,mapping.nlq[c].vdr_in_max,mapping.nlq[c].linear_deadzone_threshold};
        for(unsigned k=0;k<3;++k)if(nlq[k]>INT64_MAX||coefficient((int64_t)nlq[k],header.coef_log2_denom,&dst->nlq[k]))return DV_UNSUPPORTED;
    }
    dv_source_dm *s=&result.source;s->depth=12;s->eotf=65535;s->full_range=1;s->space=color.signal_color_space;
    unsigned char payload[512]={0};payload[1]=color.scene_refresh_flag;
    for(unsigned i=0;i<9;++i){int64_t ycc,lms;
        if(rational(color.ycc_to_rgb_matrix[i],8192,INT16_MIN,INT16_MAX,&ycc)||
           rational(color.rgb_to_lms_matrix[i],16384,INT16_MIN,INT16_MAX,&lms))return DV_UNSUPPORTED;
        s->ycc[i]=(int32_t)ycc;s->lms[i]=(int32_t)lms;
        put16(payload+2+2*i,(uint32_t)ycc&65535u);put16(payload+32+2*i,(uint32_t)lms&65535u);
    }
    for(unsigned i=0;i<3;++i){int64_t offset;
        if(rational(color.ycc_to_rgb_offset[i],INT64_C(268435456),0,UINT32_MAX,&offset))return DV_UNSUPPORTED;
        s->offset[i]=(uint32_t)offset;put32(payload+20+4*i,s->offset[i]);}
    put16(payload+50,s->eotf);payload[60]=12;payload[61]=(unsigned char)s->space;payload[62]=1;payload[63]=1;
    put16(payload+64,color.source_min_pq);put16(payload+66,color.source_max_pq);put16(payload+68,color.source_diagonal);
    size_t used=71;int area_seen=0;
    for(int i=0;i<metadata.num_ext_blocks;++i){AVDOVIDmData ext={0};
        const unsigned char *block=raw+metadata.ext_block_offset+(size_t)i*metadata.ext_block_size;
        memcpy(&ext,block,EXT_PREFIX);
        uint16_t fields[7];unsigned count;
        switch(ext.level){
        case 1:count=3;fields[0]=ext.l1.min_pq;fields[1]=ext.l1.max_pq;fields[2]=ext.l1.avg_pq;break;
        case 2:count=7;fields[0]=ext.l2.target_max_pq;fields[1]=ext.l2.trim_slope;fields[2]=ext.l2.trim_offset;
            fields[3]=ext.l2.trim_power;fields[4]=ext.l2.trim_chroma_weight;fields[5]=ext.l2.trim_saturation_gain;fields[6]=(uint16_t)ext.l2.ms_weight;break;
        case 4:count=2;fields[0]=ext.l4.anchor_pq;fields[1]=ext.l4.anchor_power;break;
        case 5:count=4;fields[0]=ext.l5.left_offset;fields[1]=ext.l5.right_offset;fields[2]=ext.l5.top_offset;fields[3]=ext.l5.bottom_offset;
            if(area_seen++||(unsigned)fields[0]+fields[1]>=width||(unsigned)fields[2]+fields[3]>=height)return DV_INVALID;
            result.active[0]=fields[0];result.active[1]=fields[2];result.active[2]=width-fields[1];result.active[3]=height-fields[3];break;
        case 6:continue;
        case 3:case 8:case 9:case 10:case 11:case 254:{
#ifdef DV_FFMPEG_RAW_EXTENSIONS
            if(metadata.ext_block_size<sizeof(ext))return DV_UNSUPPORTED;
            memcpy(&ext,block,sizeof(ext));
            if(ext.dvbridge_raw_magic!=UINT32_C(0x41424456))return DV_UNSUPPORTED;
            unsigned char converted[32];size_t converted_bytes=0;
            if(dv_cm4_extension(ext.level,ext.dvbridge_original_bytes,
                ext.dvbridge_original_length,converted,sizeof(converted),&converted_bytes))return DV_UNSUPPORTED;
            if(used+5+converted_bytes>sizeof(payload))return DV_UNSUPPORTED;
            put32(payload+used,(uint32_t)converted_bytes);payload[used+4]=ext.level;
            memcpy(payload+used+5,converted,converted_bytes);used+=5+converted_bytes;++payload[70];
            continue;
#else
            return DV_UNSUPPORTED;
#endif
        }
        default:return DV_UNSUPPORTED;
        }
        if(used+5+count*2>sizeof(payload))return DV_UNSUPPORTED;
        put32(payload+used,count*2);payload[used+4]=ext.level;used+=5;
        for(unsigned k=0;k<count;++k){put16(payload+used,fields[k]);used+=2;}++payload[70];
    }
    *out=result;memcpy(source_dm,payload,used);*source_bytes=used;
    if(single)*single=single_result;
    return DV_OK;
}

dv_status dv_settings_from_ffmpeg(const void *data,size_t bytes,unsigned width,unsigned height,
    dv_identity identity,dv_frame_settings *out,unsigned char source_dm[512],size_t *source_bytes)
{
    return settings_from_ffmpeg(data,bytes,width,height,identity,out,source_dm,source_bytes,NULL,0,10);
}
dv_status dv_transport_settings_from_ffmpeg(const void *data,size_t bytes,unsigned width,unsigned height,
    dv_identity identity,dv_frame_settings *out,unsigned char source_dm[512],size_t *source_bytes)
{
    return settings_from_ffmpeg(data,bytes,width,height,identity,out,source_dm,source_bytes,NULL,1,10);
}

dv_status dv_single_settings_from_ffmpeg_for_depth(const void *data,size_t bytes,unsigned width,unsigned height,
    unsigned sample_depth,
    dv_identity identity,dv_single_frame_settings *out,unsigned char source_dm[512],size_t *source_bytes)
{
    if(!out||!source_dm||!source_bytes)return DV_INVALID;
    if(sample_depth!=8&&sample_depth!=10)return DV_UNSUPPORTED;
    dv_frame_settings common;dv_single_frame_settings result={0};
    unsigned char payload[512];size_t used=0;
    dv_status status=settings_from_ffmpeg(data,bytes,width,height,identity,&common,payload,&used,&result.mapping,1,sample_depth);
    if(status!=DV_OK)return status;
    if(dv_single_reshape_narrow_mask(&result.mapping,result.narrow_mask))return DV_UNSUPPORTED;
    result.identity=common.identity;result.source=common.source;
    memcpy(result.active,common.active,sizeof(result.active));
    *out=result;memcpy(source_dm,payload,used);*source_bytes=used;return DV_OK;
}
dv_status dv_single_settings_from_ffmpeg(const void *data,size_t bytes,unsigned width,unsigned height,
    dv_identity identity,dv_single_frame_settings *out,unsigned char source_dm[512],size_t *source_bytes)
{return dv_single_settings_from_ffmpeg_for_depth(data,bytes,width,height,10,identity,out,source_dm,source_bytes);}
