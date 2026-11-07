#include "source_geometry.h"
#include <string.h>
static int destination_valid(const dv_source_geometry *g,unsigned width,unsigned height)
{
    return g&&g->chroma_siting<=DV_SOURCE_CHROMA_TOP_LEFT&&g->width&&g->height&&g->canvas_width&&g->canvas_height&&
        g->width<=4096&&g->height<=4096&&g->canvas_width<=4096&&g->canvas_height<=4096&&
        !(g->width%4)&&!(g->height%4)&&!(g->canvas_width%4)&&!(g->canvas_height%4)&&
        width&&height&&!(width%4)&&!(height%4)&&
        !(g->x%2)&&!(g->y%2)&&width<=g->canvas_width&&height<=g->canvas_height&&
        g->x<=g->canvas_width-width&&g->y<=g->canvas_height-height;
}
int dv_source_geometry_valid(const dv_source_geometry *g)
{return g&&(!g->destination_width==!g->destination_height)&&
    destination_valid(g,dv_source_destination_width(g),dv_source_destination_height(g));}
unsigned dv_source_destination_width(const dv_source_geometry *g)
{return g->destination_width?g->destination_width:g->width;}
unsigned dv_source_destination_height(const dv_source_geometry *g)
{return g->destination_height?g->destination_height:g->height;}
int dv_source_map_active(const dv_source_geometry *g,unsigned width,unsigned height,
                         const uint32_t in[4],uint32_t out[4])
{
    if(!destination_valid(g,width,height)||!in||!out||in[0]>=in[2]||in[1]>=in[3]||
       in[2]>g->width||in[3]>g->height)return -1;
    uint32_t mapped[4]={g->x+in[0]*width/g->width,g->y+in[1]*height/g->height,
        g->x+(in[2]*width+g->width-1)/g->width,
        g->y+(in[3]*height+g->height-1)/g->height};
    memcpy(out,mapped,sizeof(mapped));return 0;
}
static unsigned get16(const unsigned char *p){return (unsigned)p[0]*256u+p[1];}
static void put16(unsigned char *p,unsigned v){p[0]=(unsigned char)(v>>8);p[1]=(unsigned char)v;}
int dv_source_overlay_metadata(unsigned char payload[512],size_t bytes,int visible,int previous)
{
    if(!payload||bytes<71||bytes>512)return -1;
    size_t at=71,area=0;
    for(unsigned i=0;i<payload[70];++i){
        if(at>bytes||bytes-at<5)return -1;
        uint32_t n=(uint32_t)payload[at]<<24|(uint32_t)payload[at+1]<<16|
                   (uint32_t)payload[at+2]<<8|payload[at+3];
        if(n>bytes-at-5)return -1;
        if(payload[at+4]==5){if(area||n!=8)return -1;area=at+5;}
        at+=5+n;
    }
    if(at!=bytes)return -1;
    if(visible&&area)memset(payload+area,0,8);
    if(!!visible!=!!previous)payload[1]=1;
    return 0;
}
static int place_scaled(const dv_source_geometry *g,unsigned width,unsigned height,
                        unsigned char payload[512],size_t *bytes,int always_l5)
{
    if(!destination_valid(g,width,height)||!payload||!bytes||*bytes<71||*bytes>512)return -1;
    unsigned char copy[512];memcpy(copy,payload,*bytes);size_t at=71,area=0;
    for(unsigned i=0;i<copy[70];++i){if(at>*bytes||*bytes-at<5)return -1;
        uint32_t n=(uint32_t)copy[at]<<24|(uint32_t)copy[at+1]<<16|(uint32_t)copy[at+2]<<8|copy[at+3];
        if(n>*bytes-at-5)return -1;
        if(copy[at+4]==5){if(area||n!=8)return -1;area=at+5;}
        at+=5+n;
    }
    if(at!=*bytes)return -1;
    unsigned left=0,right=0,top=0,bottom=0;
    if(area){left=get16(copy+area);right=get16(copy+area+2);top=get16(copy+area+4);bottom=get16(copy+area+6);}
    if(left+right>=g->width||top+bottom>=g->height)return -1;
    uint32_t active[4]={left,top,g->width-right,g->height-bottom},placed[4];
    if(dv_source_map_active(g,width,height,active,placed))return -1;
    if((area||!always_l5)&&placed[0]==left&&placed[1]==top&&g->canvas_width-placed[2]==right&&
       g->canvas_height-placed[3]==bottom)return 0;
    if(!area){if(at>512-13||copy[70]==255)return -1;
        memset(copy+at,0,13);copy[at+3]=8;copy[at+4]=5;area=at+5;at+=13;++copy[70];}
    put16(copy+area,placed[0]);put16(copy+area+2,g->canvas_width-placed[2]);
    put16(copy+area+4,placed[1]);put16(copy+area+6,g->canvas_height-placed[3]);
    memcpy(payload,copy,at);*bytes=at;return 0;
}
int dv_source_place_scaled_metadata(const dv_source_geometry *g,unsigned width,unsigned height,
                                    unsigned char payload[512],size_t *bytes)
{return place_scaled(g,width,height,payload,bytes,0);}
/* CB1's serializer: extension blocks by level, the DM v2 blocks (L3, L8-L11,
 * L254) after the others, ties in stream order (dvbridge_metadata.h
 * cb1_wire_block); then placed, with an L5 even when the source has none. */
static int cb1_order(unsigned char payload[512],size_t bytes)
{
    if(bytes<71||bytes>512)return -1;
    size_t start[256],len[256];unsigned rank[256],order[256],count=payload[70],at=71;
    for(unsigned i=0;i<count;++i){if(at>bytes||bytes-at<5)return -1;
        uint32_t n=(uint32_t)payload[at]<<24|(uint32_t)payload[at+1]<<16|(uint32_t)payload[at+2]<<8|payload[at+3];
        if(n>bytes-at-5)return -1;
        unsigned level=payload[at+4],r=level+((level==3||(level>=8&&level<=11)||level==254)?256u:0u);
        start[i]=at;len[i]=5+n;at+=5+n;
        unsigned k=i;while(k>0&&rank[k-1]>r){rank[k]=rank[k-1];order[k]=order[k-1];--k;}
        rank[k]=r;order[k]=i;}
    if(at!=bytes)return -1;
    unsigned char copy[512];memcpy(copy,payload,bytes);at=71;
    for(unsigned k=0;k<count;++k){memcpy(payload+at,copy+start[order[k]],len[order[k]]);at+=len[order[k]];}
    return 0;
}
int dv_source_place_metadata_cb1(const dv_source_geometry *g,unsigned char payload[512],size_t *bytes)
{
    if(!dv_source_geometry_valid(g)||!payload||!bytes||cb1_order(payload,*bytes))return -1;
    return place_scaled(g,dv_source_destination_width(g),dv_source_destination_height(g),payload,bytes,1);
}
unsigned dv_packet_sequence_next(dv_packet_sequence *s,const unsigned char *payload,size_t bytes)
{
    if(!s||!payload||bytes>sizeof(s->previous))return 0;
    if(s->frames&&(s->size!=bytes||memcmp(s->previous,payload,bytes)))s->id=(s->id+1)&15u;
    memcpy(s->previous,payload,bytes);s->size=bytes;s->frames++;
    return s->id;
}
int dv_source_place_metadata(const dv_source_geometry *g,unsigned char payload[512],size_t *bytes)
{
    if(!dv_source_geometry_valid(g))return -1;
    return dv_source_place_scaled_metadata(g,dv_source_destination_width(g),dv_source_destination_height(g),payload,bytes);
}
