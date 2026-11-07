#include "colour_lut.h"
#include <math.h>
static void pair(float *p,double value){p[0]=(float)value;p[1]=(float)(value-(double)p[0]);}
static double decode(double v){if(v<0)v=0;double p=pow(v,32.0/2523.0),d=2413.0/128.0-2392.0/128.0*p;return pow(fmax(p-3424.0/4096.0,0)/d,16384.0/2610.0);}
static double encode(double v){double p=pow(fmax(v,0),2610.0/16384.0);return pow((3424.0/4096.0+2413.0/128.0*p)/(1+2392.0/128.0*p),2523.0/32.0);}
size_t dv_decode_lut_count(void){return 4*(size_t)(DV_DECODE_LAST+1);}
size_t dv_encode_lut_count(void){return 1+4*(size_t)(DV_ENCODE_LAST_EXP-DV_ENCODE_FIRST_EXP+1)*DV_ENCODE_STEPS;}
static void polynomial(float *out,double a,double b,double c,double d)
{
    double values[4]={a,b-a,(c-2*b+a)/2,(d-3*c+3*b-a)/6};
    for(unsigned k=0;k<4;++k){out[k]=(float)values[k];out[4+k]=(float)(values[k]-(double)out[k]);}
}
int dv_colour_make_luts(float *d,float *e)
{
    if(!d || !e)return -1;
    double a=decode(-1.0/65536),b=decode(0),c=decode(1.0/65536);
    for(int i=0;i<=DV_DECODE_LAST;++i){double next=decode((double)(i+2)/65536);
        polynomial(d+8*(size_t)i,a,b,c,next);a=b;b=c;c=next;}
    pair(e,encode(0));
    for(int exp=DV_ENCODE_FIRST_EXP;exp<=DV_ENCODE_LAST_EXP;++exp){
        a=encode(ldexp(1-1.0/DV_ENCODE_STEPS,exp));b=encode(ldexp(1,exp));c=encode(ldexp(1+1.0/DV_ENCODE_STEPS,exp));
        for(int i=0;i<DV_ENCODE_STEPS;++i){double next=encode(ldexp(1+(double)(i+2)/DV_ENCODE_STEPS,exp));
            size_t offset=1+4*((size_t)(exp-DV_ENCODE_FIRST_EXP)*DV_ENCODE_STEPS+(size_t)i);
            polynomial(e+2*offset,a,b,c,next);a=b;b=c;c=next;
        }
    }
    return 0;
}
