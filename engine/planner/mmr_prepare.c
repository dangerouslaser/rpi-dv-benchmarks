#include "mmr_prepare.h"
#include <string.h>
int dv_reference_chroma_batch(const dv_intel_composer_config *,unsigned,size_t,const uint16_t *,const uint16_t *,const uint16_t *,const uint16_t *,uint16_t *,int32_t *,int32_t *,uint16_t *);
static __int128 coefficient(dv_exact_coefficient c,unsigned d)
{return (__int128)c.integer*((__int128)1<<d)+c.fraction;}
static void words(uint32_t *out,__int128 value)
{unsigned __int128 bits=(unsigned __int128)value;for(unsigned i=0;i<4;++i)out[i]=(uint32_t)(bits>>(32*i));}
dv_status dv_mmr_prepare(const dv_intel_composer_config *cfg,unsigned component,uint32_t params[96],int32_t table[1024])
{
    if(!cfg || !params || !table || component<1 || component>2)return DV_INVALID;
    if(cfg->component[component].mapping!=1)return DV_UNSUPPORTED;
    uint16_t zeros[1024]={0},el[1024],mapped[1024],out[1024];int32_t sum[1024],residual[1024];
    for(unsigned i=0;i<1024;++i)el[i]=(uint16_t)i;
    if(dv_reference_chroma_batch(cfg,component,1024,zeros,zeros,zeros,el,mapped,residual,sum,out))return DV_UNSUPPORTED;
    uint32_t p[96]={0};const dv_component_config *c=&cfg->component[component];unsigned d=cfg->coefficient_log2_denominator;
    p[0]=c->mmr_order;p[1]=d;
    for(unsigned k=0;k<3;++k){p[2+2*k]=cfg->component[k].pivots[0];p[3+2*k]=cfg->component[k].pivots[cfg->component[k].pivot_count-1];}
    words(p+8,coefficient(c->mmr_constant,d)*((__int128)1<<20));
    for(unsigned order=0;order<c->mmr_order;++order)for(unsigned k=0;k<7;++k)words(p+12+4*(order*7+k),coefficient(c->mmr[order][k],d));
    memcpy(params,p,sizeof(p));memcpy(table,residual,sizeof(residual));return DV_OK;
}
