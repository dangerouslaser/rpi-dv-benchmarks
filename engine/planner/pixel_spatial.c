/* Private spatial test reference; backend/filter choices remain explicit.
 * Enhancement path: the informative CCM Annex B example, or (big-fella)
 * nearest neighbour: every EL sample repeated over its 2x2 block.
 * MMR luma guide follows CCM 5.4.2.3.3 for left-sited 4:2:0 inputs.
 */
#include <stdint.h>
#include <stddef.h>
static unsigned edge(int p,unsigned n)
{ return p<0?0:((unsigned)p>=n?n-1:(unsigned)p); }
static uint16_t rounded(int64_t value,unsigned divisor)
{
    value+=(int64_t)divisor/2;
    value=value>=0?value/divisor:-((-value+divisor-1)/divisor);
    return value<0?0:(value>65535?65535:(uint16_t)value);
}
static int shape(const uint16_t *in,size_t count,unsigned w,unsigned h)
{
    if (!in || !w || !h || w>4096 || h>4096 || count!=(size_t)w*h) return 0;
    for (size_t i=0;i<count;++i) if (in[i]>1023) return 0;
    return 1;
}
/* Explicit historical phase experiment, NOT a normative Dolby filter.
 * Input top-left centres move to left centres by sampling row j+1/4.
 * method 0 = linear, method 1 = four-tap cubic; both use nominal-depth bounds.
 */
int dv_chroma_phase_left(const uint16_t *in,size_t count,unsigned w,unsigned h,
                         unsigned method,uint16_t *out,size_t out_count)
{
    if (!shape(in,count,w,h) || method>2 || !out || out_count!=count) return -1;
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) {
        uint16_t v;
        if (method==2) v=in[(size_t)y*w+x];   /* none (CB1's Master Reference) */
        else if (!method) v=rounded(3*(int64_t)in[(size_t)y*w+x]+in[(size_t)edge((int)y+1,h)*w+x],4);
        else {
            static const int c[4]={-9,111,29,-3};
            int64_t sum=0;
            for (int k=0;k<4;++k) sum+=(int64_t)c[k]*in[(size_t)edge((int)y+k-1,h)*w+x];
            v=rounded(sum,128);
        }
        out[(size_t)y*w+x]=v>1023?1023:v;
    }
    return 0;
}
/* Non-overlapping arrays are required; descriptors use element counts. */
int dv_mmr_luma_guide(const uint16_t *in,size_t count,unsigned w,unsigned h,
                      uint16_t *out,size_t out_count)
{
    if (!shape(in,count,w,h) || (w%2) || (h%2) || !out || out_count!=(size_t)(w/2)*(h/2)) return -1;
    for (unsigned y=0;y<h/2;++y) for (unsigned x=0;x<w/2;++x) {
        unsigned sums[2];
        for (unsigned r=0;r<2;++r) {
            size_t row=(size_t)(2*y+r)*w;
            unsigned ix=2*x;
            sums[r]=(in[row+edge((int)ix-1,w)]+2U*in[row+ix]+in[row+edge((int)ix+1,w)]+2U)/4U;
        }
        out[(size_t)y*(w/2)+x]=(uint16_t)((sums[0]+sums[1]+1U)/2U);
    }
    return 0;
}
int dv_annex_b_scale2x(const uint16_t *in,size_t count,unsigned w,unsigned h,
                      unsigned chroma,uint16_t *scratch,size_t scratch_count,
                      uint16_t *out,size_t out_count)
{
    if (!shape(in,count,w,h) || chroma>1 || !scratch || scratch_count!=w || !out || out_count!=4*(size_t)w*h) return -1;
    static const int taps[8]={22,94,-524,2456,2456,-524,94,22};
    for (unsigned y=0;y<2*h;++y) {
        int row=(int)(y/2);
        for (unsigned x=0;x<w;++x) {
            if (chroma) {
                int a=(y%2)?192:64,b=(y%2)?64:192;
                int r0=(y%2)?row:row-1,r1=(y%2)?row+1:row;
                scratch[x]=rounded((int64_t)a*in[(size_t)edge(r0,h)*w+x]+(int64_t)b*in[(size_t)edge(r1,h)*w+x],256);
            } else {
                static const int even[4]={-3,29,111,-9},odd[4]={-9,111,29,-3};
                const int *coefficients=(y%2)?odd:even;
                int first=(y%2)?row-1:row-2;
                int64_t value=0;
                for (int k=0;k<4;++k) value+=(int64_t)coefficients[k]*in[(size_t)edge(first+k,h)*w+x];
                scratch[x]=rounded(value,128);
            }
        }
        for (unsigned x=0;x<2*w;++x) {
            unsigned ix=x/2;
            uint16_t value=scratch[ix];
            if (x%2) {
                int64_t sum=0;
                for (int k=0;k<8;++k) sum+=(int64_t)taps[k]*scratch[edge((int)ix+k-3,w)];
                value=rounded(sum,4096);
            }
            out[(size_t)y*2*w+x]=value;
        }
    }
    return 0;
}
/* big-fella: nearest-neighbour EL upscale. The EL luma grid is co-sited
 * horizontally and centred vertically on the BL's, so output (x,y) takes EL
 * sample (x/2,y/2) exactly; chroma (left-sited after nothing is moved) the
 * same. No rounding, no ringing: the output stays within the input codes. */
int dv_nearest_scale2x(const uint16_t *in,size_t count,unsigned w,unsigned h,
                       uint16_t *out,size_t out_count)
{
    if (!shape(in,count,w,h) || !out || out_count!=4*(size_t)w*h) return -1;
    for (unsigned y=0;y<2*h;++y) for (unsigned x=0;x<2*w;++x)
        out[(size_t)y*2*w+x]=in[(size_t)(y/2)*w+x/2];
    return 0;
}
