#include "dv_cm4.h"
#include <string.h>

/* RPU grammar cross-check: quietvoid/dovi_tool extension_metadata/blocks.
 * Tunnel field widths also cross-checked against the private source-metadata
 * audit and the existing Kodi raw-extension adapter. No trim modification. */
typedef struct {const uint8_t *p;unsigned position,bits;int valid;} reader;
static unsigned get(reader *r,unsigned bits)
{
    if(bits>16||r->position>r->bits||bits>r->bits-r->position){r->valid=0;return 0;}
    unsigned value=0;
    while(bits--){value=(value<<1)|(((unsigned)r->p[r->position/8]>>(7-r->position%8))&1u);++r->position;}
    return value;
}
static void field(reader *r,unsigned bits,uint8_t out[32],size_t *size)
{
    unsigned value=get(r,bits);
    if(bits>8)out[(*size)++]=(uint8_t)(value>>8);
    out[(*size)++]=(uint8_t)value;
}
int dv_cm4_extension(unsigned level,const uint8_t *raw,size_t bytes,
                     uint8_t *output,size_t capacity,size_t *output_bytes)
{
    if(!raw||!output||!output_bytes||bytes>32)return -1;
    reader r={raw,0,(unsigned)bytes*8,1};uint8_t result[32];size_t size=0;
    switch(level){
    case 3:
        if(bytes!=5)return -1;
        for(unsigned i=0;i<3;++i)field(&r,12,result,&size);
        break;
    case 8:
        if(bytes!=10&&bytes!=12&&bytes!=13&&bytes!=19&&bytes!=25)return -1;
        field(&r,8,result,&size);
        for(unsigned i=0;i<6;++i)field(&r,12,result,&size);
        if(bytes>=12)field(&r,12,result,&size);
        if(bytes>=13)field(&r,12,result,&size);
        if(bytes>=19)for(unsigned i=0;i<6;++i)field(&r,8,result,&size);
        if(bytes>=25)for(unsigned i=0;i<6;++i)field(&r,8,result,&size);
        break;
    case 9:
        if(bytes!=1&&bytes!=17)return -1;
        field(&r,8,result,&size);
        if((result[0]==255)!=(bytes==17))return -1;
        if(bytes==17)for(unsigned i=0;i<8;++i)field(&r,16,result,&size);
        break;
    case 10:
        if(bytes!=5&&bytes!=21)return -1;
        field(&r,8,result,&size);field(&r,12,result,&size);
        field(&r,12,result,&size);field(&r,8,result,&size);
        if((result[5]==255)!=(bytes==21))return -1;
        if(bytes==21)for(unsigned i=0;i<8;++i)field(&r,16,result,&size);
        break;
    case 11:
        /* Whitepoint occupies bits0..3; reference mode is bit4. Preserve
         * byte2 verbatim, as the current RPU grammar permits it. */
        if(bytes!=4||raw[0]>15||(raw[1]&0xe0)||raw[3])return -1;
        for(unsigned i=0;i<4;++i)field(&r,8,result,&size);
        break;
    case 254:
        if(bytes!=2)return -1;
        field(&r,8,result,&size);field(&r,8,result,&size);break;
    default:return -1;
    }
    while(r.valid&&r.position<r.bits)if(get(&r,1))return -1;
    if(!r.valid||size>capacity)return -1;
    memcpy(output,result,size);*output_bytes=size;return 0;
}
