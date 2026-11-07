/* Private source-colour diagnostic. Double reference or explicit FP32 experiment,
 * selected at compile time; neither is a production playback implementation.
 * Integer metadata is decoded once; no target/display mapping is selected.
 */
#include <math.h>
#include <stdint.h>
#include "dv_colour_internal.h"
#ifndef DV_FP32_MASK
#ifdef DV_COLOUR_FP32
#define DV_FP32_MASK 127
#else
#define DV_FP32_MASK 0
#endif
#endif
#if DV_FP32_MASK & 1
typedef float ycc_pixel;
#else
typedef double ycc_pixel;
#endif
#if DV_FP32_MASK & 2
typedef float decode_pixel;
#define DEC_POW powf
#define DEC_MAX fmaxf
#else
typedef double decode_pixel;
#define DEC_POW pow
#define DEC_MAX fmax
#endif
#if DV_FP32_MASK & 4
typedef float lms_pixel;
#else
typedef double lms_pixel;
#endif
#if DV_FP32_MASK & 8
typedef float target_pixel;
#else
typedef double target_pixel;
#endif
#if DV_FP32_MASK & 16
typedef float encode_pixel;
#define ENC_POW powf
#define ENC_MAX fmaxf
#else
typedef double encode_pixel;
#define ENC_POW pow
#define ENC_MAX fmax
#endif
#if DV_FP32_MASK & 32
typedef float transport_pixel;
#else
typedef double transport_pixel;
#endif
#if DV_FP32_MASK & 64
typedef float quant_pixel;
#define QUANT_FLOOR floorf
#else
typedef double quant_pixel;
#define QUANT_FLOOR floor
#endif

/* policy 0 rejects outside PQ's unit domain. Policy 1 is the historical
 * diagnostic extension: negative to zero, positive beyond one retained.
 * Neither policy is claimed to reproduce licensed hardware outside [0,1].
 */
int dv_source_colour_init(const dv_source_dm *dm, uint32_t policy, dv_source_colour *out)
{
    if (!dm || !out || policy>1 || dm->compressed || dm->eotf!=65535 ||
        dm->param[0] || dm->param[1] || dm->param[2] || dm->depth!=12 ||
        dm->space || dm->chroma || dm->full_range!=1) return -1;
    dv_source_colour c;
    for (unsigned i=0;i<9;++i) {
        if (dm->ycc[i]<-32768 || dm->ycc[i]>32767 || dm->lms[i]<-32768 || dm->lms[i]>32767) return -1;
        c.ycc[i]=(double)dm->ycc[i]/8192.0;
        c.lms[i]=(double)dm->lms[i]/16384.0;
    }
    for (unsigned i=0;i<3;++i) c.offset[i]=(double)dm->offset[i]/268435456.0;
    c.policy=policy;
    *out=c;
    return 0;
}
static int decode(double value,uint32_t policy,double *out)
{
    if (!isfinite(value) || policy>1 || (!policy && (value<0.0 || value>1.0))) return -1;
    if (value<0.0) value=0.0;
    decode_pixel p=DEC_POW((decode_pixel)value,(decode_pixel)(32.0/2523.0));
    decode_pixel divisor=(decode_pixel)(2413.0/128.0)-(decode_pixel)(2392.0/128.0)*p;
    if (divisor<=0.0) return -1;
    decode_pixel value_linear=DEC_POW(DEC_MAX(p-(decode_pixel)(3424.0/4096.0),(decode_pixel)0)/divisor,(decode_pixel)(16384.0/2610.0));
    if (!isfinite(value_linear)) return -1;
    *out=value_linear;
    return 0;
}
int dv_source_colour_sample(const dv_source_colour *c,const double input[3],dv_source_result *out)
{
    if (!c || !input || !out || c->policy>1) return -1;
    ycc_pixel shifted[3];
    for (unsigned i=0;i<3;++i) {
        if (!isfinite(input[i]) || input[i]<0.0 || input[i]>4095.0 || !isfinite(c->offset[i])) return -1;
        shifted[i]=(ycc_pixel)input[i]/(ycc_pixel)4096-(ycc_pixel)c->offset[i];
    }
    dv_source_result r;
    for (unsigned i=0;i<3;++i) {
        ycc_pixel v=0;
        for (unsigned k=0;k<3;++k) v+=(ycc_pixel)c->ycc[3*i+k]*shifted[k];
        r.nonlinear[i]=v;
        if (decode(r.nonlinear[i],c->policy,&r.linear_rgb[i])) return -1;
    }
    for (unsigned i=0;i<3;++i) {
        lms_pixel v=0;
        for (unsigned k=0;k<3;++k) v+=(lms_pixel)c->lms[3*i+k]*(lms_pixel)r.linear_rgb[k];
        r.linear_lms[i]=v;
        if (!isfinite(r.linear_lms[i])) return -1;
    }
    *out=r;
    return 0;
}

static int inverse(const double m[9],double out[9])
{
    for (unsigned i=0;i<9;++i) if (!isfinite(m[i])) return -1;
    double a=m[0],b=m[1],c=m[2],d=m[3],e=m[4],f=m[5],g=m[6],h=m[7],i=m[8];
    double v[9]={e*i-f*h,c*h-b*i,b*f-c*e,f*g-d*i,a*i-c*g,c*d-a*f,d*h-e*g,b*g-a*h,a*e-b*d};
    double determinant=a*v[0]+b*v[3]+c*v[6];
    if (!isfinite(determinant) || determinant==0.0) return -1;
    for (unsigned k=0;k<9;++k) {
        out[k]=v[k]/determinant;
        if (!isfinite(out[k])) return -1;
    }
    return 0;
}
/* Target supplied explicitly; no EDID, target preset, trims or tone mapping. */
int dv_target_colour_init(const double ycc[9],const double lms[9],const double offset[3],dv_target_colour *out)
{
    if (!ycc || !lms || !offset || !out) return -1;
    dv_target_colour c;
    if (inverse(ycc,c.inverse_ycc) || inverse(lms,c.inverse_lms)) return -1;
    for (unsigned i=0;i<3;++i) {
        if (!isfinite(offset[i])) return -1;
        c.offset[i]=offset[i];
    }
    *out=c;
    return 0;
}
int dv_target_colour_sample(const dv_target_colour *c,uint32_t policy,
                           const double common_lms[3],dv_target_result *out)
{
    if (!c || !common_lms || !out || policy>1) return -1;
    for (unsigned i=0;i<3;++i) if (!isfinite(common_lms[i])) return -1;
    dv_target_result r;
    encode_pixel encoded[3];
    for (unsigned i=0;i<3;++i) {
        target_pixel v=0;
        for (unsigned k=0;k<3;++k) v+=(target_pixel)c->inverse_lms[3*i+k]*(target_pixel)common_lms[k];
        r.linear[i]=v;
        if (!isfinite(r.linear[i]) || (!policy && (r.linear[i]<0.0 || r.linear[i]>1.0))) return -1;
        encode_pixel p=ENC_POW(ENC_MAX((encode_pixel)v,(encode_pixel)0),(encode_pixel)(2610.0/16384.0));
        encoded[i]=ENC_POW(((encode_pixel)(3424.0/4096.0)+(encode_pixel)(2413.0/128.0)*p)/((encode_pixel)1+(encode_pixel)(2392.0/128.0)*p),(encode_pixel)(2523.0/32.0));
        if (!isfinite(encoded[i])) return -1;
    }
    for (unsigned i=0;i<3;++i) {
        transport_pixel v=(transport_pixel)c->offset[i];
        for (unsigned k=0;k<3;++k) v+=(transport_pixel)c->inverse_ycc[3*i+k]*(transport_pixel)encoded[k];
        r.transport[i]=v;
        if (!isfinite(r.transport[i])) return -1;
        quant_pixel code=QUANT_FLOOR((quant_pixel)v*(quant_pixel)4096+(quant_pixel)0.5);
        if (code<0.0) code=0.0;
        if (code>4095.0) code=4095.0;
        r.code[i]=(uint16_t)code;
    }
    *out=r;
    return 0;
}
