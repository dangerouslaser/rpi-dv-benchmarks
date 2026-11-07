#include "single_reshape.h"
#include <string.h>
typedef __int128 wide;
static int valid(const dv_single_reshape *m)
{
    if(!m||m->base_depth<8||m->base_depth>12||m->denominator>32)return 0;
    unsigned limit=(1u<<m->base_depth)-1;
    for(unsigned c=0;c<3;++c){const dv_reshape_curve *v=&m->curve[c];
        if(v->pivot_count<2||v->pivot_count>9)return 0;
        for(unsigned p=0;p<v->pivot_count;++p)
            if(v->pivots[p]>limit||(p&&v->pivots[p]<=v->pivots[p-1]))return 0;
        for(unsigned p=0;p+1<v->pivot_count;++p){const dv_reshape_piece *s=&v->piece[p];
            if(s->method>1||s->order<1||s->order>(s->method?3u:2u))return 0;}
    }
    return 1;
}
int dv_single_reshape_narrow_mask(const dv_single_reshape *m,uint32_t masks[3])
{
    if(!valid(m)||!masks)return -1;
    uint32_t result[3]={0};wide unit=(wide)1<<(2*m->base_depth);
    for(unsigned c=0;c<3;++c)for(unsigned p=0;p+1<m->curve[c].pivot_count;++p){
        const dv_reshape_piece *piece=&m->curve[c].piece[p];
        wide coefficient=piece->constant,bound=(coefficient<0?-coefficient:coefficient)*unit;
        unsigned orders=piece->method?piece->order:1,terms=piece->method?7:piece->order;
        for(unsigned order=0;order<orders;++order)for(unsigned k=0;k<terms;++k){
            coefficient=piece->coefficient[order][k];bound+=(coefficient<0?-coefficient:coefficient)*unit;}
        if(bound<=INT64_MAX)result[c]|=1u<<p;
    }
    memcpy(masks,result,sizeof(result));return 0;
}
int dv_single_reshape_surface_masks(const dv_single_reshape *m,unsigned depth,uint32_t masks[3])
{
    if((depth!=8&&depth!=10)||!valid(m)||m->base_depth!=depth||!masks)return -1;
    for(unsigned p=0;p+1<m->curve[0].pivot_count;++p)
        if(m->curve[0].piece[p].method)return -1;
    return dv_single_reshape_narrow_mask(m,masks);
}
int dv_single_reshape_p010_masks(const dv_single_reshape *m,uint32_t masks[3])
{return dv_single_reshape_surface_masks(m,10,masks);}
int dv_single_reshape_order2_chroma(const dv_single_reshape *m)
{
    uint32_t masks[3];if(dv_single_reshape_p010_masks(m,masks))return 0;
    for(unsigned c=1;c<3;++c){const dv_reshape_curve *v=&m->curve[c];
        if(v->pivot_count!=2||v->piece[0].method!=1||v->piece[0].order!=2||!(masks[c]&1u))return 0;}
    return 1;
}
int dv_single_reshape_needs_luma_guide(const dv_single_reshape *m)
{
    if(!valid(m))return -1;
    const unsigned terms[4]={0,3,4,6};
    for(unsigned c=1;c<3;++c)for(unsigned p=0;p+1<m->curve[c].pivot_count;++p){
        const dv_reshape_piece *piece=&m->curve[c].piece[p];if(!piece->method)continue;
        for(unsigned order=0;order<piece->order;++order)for(unsigned k=0;k<4;++k)
            if(piece->coefficient[order][terms[k]])return 1;
    }
    return 0;
}
int dv_single_reshape_reference(const dv_single_reshape *m,const uint16_t input[3],uint16_t output[3])
{
    if(!valid(m)||!input||!output)return -1;
    unsigned s[3],pieces[3];uint16_t result[3];unsigned bits=m->base_depth,q=2*bits;
    for(unsigned c=0;c<3;++c){const dv_reshape_curve *v=&m->curve[c];
        if(input[c]>=(1u<<bits))return -1;
        pieces[c]=v->pivot_count-2;
        for(unsigned p=0;p+1<v->pivot_count;++p)if(input[c]<v->pivots[p+1]){pieces[c]=p;break;}
        s[c]=input[c]<v->pivots[0]?v->pivots[0]:input[c];
        if(s[c]>v->pivots[v->pivot_count-1])s[c]=v->pivots[v->pivot_count-1];
    }
    uint64_t feature[3][7]={{0}};
    for(unsigned c=0;c<3;++c){feature[0][c]=(uint64_t)s[c]<<bits;feature[1][c]=(uint64_t)s[c]*s[c];}
    feature[0][3]=(uint64_t)s[0]*s[1];feature[0][4]=(uint64_t)s[0]*s[2];feature[0][5]=(uint64_t)s[1]*s[2];
    feature[0][6]=(feature[0][3]*feature[0][2])>>q;
    for(unsigned k=3;k<7;++k)feature[1][k]=(feature[0][k]*feature[0][k])>>q;
    for(unsigned k=0;k<7;++k)feature[2][k]=(feature[0][k]*feature[1][k])>>q;
    for(unsigned c=0;c<3;++c){const dv_reshape_piece *p=&m->curve[c].piece[pieces[c]];
        wide value=(wide)p->constant*((wide)1<<q);
        if(!p->method){value+=(wide)p->coefficient[0][0]*s[c]*((wide)1<<bits);
            if(p->order==2)value+=(wide)p->coefficient[0][1]*s[c]*s[c];
        }else for(unsigned order=0;order<p->order;++order)
            for(unsigned k=0;k<7;++k)value+=(wide)p->coefficient[order][k]*feature[order][k];
        if(value<0)value=0;
        value>>=m->denominator+q-16;
        if(value>65535)value=65535;
        value=(value+8)>>4;if(value>4095)value=4095;result[c]=(uint16_t)value;
    }
    memcpy(output,result,sizeof(result));return 0;
}
int dv_single_reshape_luma_table_for_depth(const dv_single_reshape *m,unsigned depth,
                                         uint16_t *output,size_t entries)
{
    uint32_t masks[3];uint16_t table[1024];
    if(!output||dv_single_reshape_surface_masks(m,depth,masks)||entries!=((size_t)1<<depth))return -1;
    for(unsigned i=0;i<entries;++i){uint16_t in[3]={(uint16_t)i,0,0},out[3];
        if(dv_single_reshape_reference(m,in,out))return -1;
        table[i]=out[0];}
    memcpy(output,table,entries*sizeof(*table));return 0;
}
int dv_single_reshape_luma_table(const dv_single_reshape *m,uint16_t output[1024])
{return dv_single_reshape_luma_table_for_depth(m,10,output,1024);}
