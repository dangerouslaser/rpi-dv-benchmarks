/* Integer chroma reference. No sampling/scaling/display mapping.
 * 10/10/12-bit polynomial or one MMR interval per chroma component.
 * __int128 keeps all supported coefficient/intermediate products bounded.
 */
#include "intel_composer_config.h"
#include "pixel_polynomial.h"
#include <stddef.h>
#include <stdint.h>
#if defined(DV_BOUNDED_MMR64) && !defined(DV_PREPARED_CHROMA)
#error Guarded MMR64 requires exact prepared coefficient and residual tables
#endif
typedef __int128 wide;
static wide coeff(dv_exact_coefficient v, unsigned d)
{ return (wide)v.integer * ((wide)1 << d) + v.fraction; }
static wide floor_div(wide v, unsigned n)
{
    wide divisor = (wide)1 << n;
    return v >= 0 ? v/divisor : -((-v+divisor-1)/divisor);
}
static int valid(const dv_intel_composer_config *c, unsigned component)
{
    if (!c || c->abi_version != 1 || component < 1 || component > 2 ||
        c->base_depth != 10 || c->enhancement_depth != 10 ||
        c->reconstruction_depth != 12 || c->residual_enabled > 1 ||
        c->coefficient_log2_denominator < 15 || c->coefficient_log2_denominator > 32)
        return 0;
    unsigned d = c->coefficient_log2_denominator;
    for (unsigned i = 0; i < 3; ++i) {
        const dv_component_config *v = &c->component[i];
        if (v->pivot_count < 2 || v->pivot_count > 9) return 0;
        for (unsigned k = 0; k < v->pivot_count; ++k)
            if (v->pivots[k] > 1023 || (k && v->pivots[k] <= v->pivots[k-1])) return 0;
    }
    const dv_component_config *v = &c->component[component];
    if (v->mapping != 1 || v->pivot_count != 2 || v->mmr_order < 1 ||
        v->mmr_order > 3 || v->nlq_offset > 1023 ||
        (uint64_t)v->mmr_constant.fraction >= (UINT64_C(1)<<d)) return 0;
    for (unsigned i = 0; i < v->mmr_order; ++i)
        for (unsigned k = 0; k < 7; ++k)
            if ((uint64_t)v->mmr[i][k].fraction >= (UINT64_C(1)<<d)) return 0;
    for (unsigned i = 0; i < 3; ++i)
        if ((uint64_t)v->nlq[i].fraction >= (UINT64_C(1)<<d) ||
            coeff(v->nlq[i],d) < 0 || coeff(v->nlq[i],d) >= ((wide)2<<d)) return 0;
    return 1;
}

/* Inputs are explicit, already-aligned integer code planes. Validation happens
 * before output writes; callers must provide non-overlapping, count-sized
 * arrays. This API neither chooses nor constructs the MMR luma guide.
 */
int dv_reference_chroma_batch(const dv_intel_composer_config *cfg, unsigned component,
    size_t count, const uint16_t *y, const uint16_t *cb, const uint16_t *cr,
    const uint16_t *el, uint16_t *mapped, int32_t *residual, int32_t *sum,
    uint16_t *output)
{
    if(cfg&&component>=1&&component<=2&&!cfg->component[component].mapping){
        if(!y||!cb||!cr||!el||!mapped||!residual||!sum||!output||
           count>SIZE_MAX/sizeof(int32_t))return -1;
        int32_t v,r;uint16_t code;
        if(dv_reference_polynomial(cfg,component,0,0,&v,&r,&code))return -1;
        for(size_t i=0;i<count;++i)
            if(y[i]>1023||cb[i]>1023||cr[i]>1023||el[i]>1023)return -1;
        const uint16_t *base=component==1?cb:cr;
        for(size_t i=0;i<count;++i){
            if(dv_reference_polynomial(cfg,component,base[i],el[i],&v,&r,&code))return -1;
            mapped[i]=(uint16_t)v;residual[i]=r;sum[i]=(int32_t)((int64_t)v+r);output[i]=code;
        }
        return 0;
    }
    if (!valid(cfg,component) || !y || !cb || !cr || !el || !mapped ||
        !residual || !sum || !output || count > SIZE_MAX/sizeof(int32_t)) return -1;
    for (size_t i = 0; i < count; ++i)
        if (y[i] > 1023 || cb[i] > 1023 || cr[i] > 1023 || el[i] > 1023) return -1;
    unsigned d = cfg->coefficient_log2_denominator;
    const dv_component_config *c = &cfg->component[component];
#ifdef DV_PREPARED_CHROMA
    wide constant=coeff(c->mmr_constant,d)*((wide)1<<20);
    int64_t coefficients[3][7]={{0}};
    for (unsigned order=0;order<c->mmr_order;++order)
        for (unsigned k=0;k<7;++k) coefficients[order][k]=(int64_t)coeff(c->mmr[order][k],d);
#ifdef DV_BOUNDED_MMR64
    /* Every nonnegative feature is < 2^20. This absolute-sum bound proves
     * ALL products and ALL partial sums fit int64, independent of samples.
     * Extreme coefficients retain the unchanged signed-128-bit fallback.
     */
    wide bound=constant<0?-constant:constant;
    for (unsigned order=0;order<c->mmr_order;++order) for (unsigned k=0;k<7;++k) {
        wide value=coefficients[order][k];if (value<0) value=-value;
        bound+=value*((wide)1<<20);
    }
    int narrow=bound<=INT64_MAX;
#endif
    int32_t residual_table[1024];
    wide slope=coeff(c->nlq[0],d),maximum=2*coeff(c->nlq[1],d),threshold=coeff(c->nlq[2],d);
    for (unsigned code=0;code<1024;++code) {
        int q=(int)code-(int)c->nlq_offset;wide r=0;
        if (cfg->residual_enabled && q) {
            int sign=q<0?-1:1;wide dq=(2*q-sign)*slope+2*sign*threshold;
            if (dq>maximum) dq=maximum;if (dq < -maximum) dq= -maximum;
            r=floor_div(dq,d-15);
        }
        residual_table[code]=(int32_t)r;
    }
#endif
    for (size_t i = 0; i < count; ++i) {
        uint32_t s[3] = {y[i],cb[i],cr[i]};
        for (unsigned k = 0; k < 3; ++k) {
            const dv_component_config *v = &cfg->component[k];
            if (s[k] < v->pivots[0]) s[k] = v->pivots[0];
            if (s[k] > v->pivots[v->pivot_count-1]) s[k] = v->pivots[v->pivot_count-1];
        }
#ifdef DV_PREPARED_CHROMA
        /* Valid 10-bit inputs give each feature < 2^20. Feature products
         * therefore fit unsigned 64 bits exactly; weighted accumulation
         * deliberately remains signed 128 bits for extreme coefficients.
         */
        uint64_t t[3][7]={{0}};
        for (unsigned k=0;k<3;++k) {t[0][k]=(uint64_t)s[k]<<10;t[1][k]=(uint64_t)s[k]*s[k];}
        t[0][3]=(uint64_t)s[0]*s[1];t[0][4]=(uint64_t)s[0]*s[2];t[0][5]=(uint64_t)s[1]*s[2];
        t[0][6]=(t[0][3]*t[0][2])>>20;
        for (unsigned k=3;k<7;++k) t[1][k]=(t[0][k]*t[0][k])>>20;
        for (unsigned k=0;k<7;++k) t[2][k]=(t[0][k]*t[1][k])>>20;
#else
        wide t[3][7] = {{0}};
        for (unsigned k = 0; k < 3; ++k) {
            t[0][k] = (wide)s[k]<<10;
            t[1][k] = (wide)s[k]*s[k];
        }
        t[0][3] = (wide)s[0]*s[1];
        t[0][4] = (wide)s[0]*s[2];
        t[0][5] = (wide)s[1]*s[2];
        t[0][6] = floor_div(t[0][3]*t[0][2],20);
        for (unsigned k = 3; k < 7; ++k) t[1][k] = floor_div(t[0][k]*t[0][k],20);
        for (unsigned k = 0; k < 7; ++k) t[2][k] = floor_div(t[0][k]*t[1][k],20);
#endif
        wide v;
#ifdef DV_BOUNDED_MMR64
        if (narrow) {
            int64_t value=(int64_t)constant;
            for (unsigned order=0;order<c->mmr_order;++order)
                for (unsigned k=0;k<7;++k) value+=coefficients[order][k]*(int64_t)t[order][k];
            v=value;
        } else {
#endif
        v =
#ifdef DV_PREPARED_CHROMA
            constant;
#else
            coeff(c->mmr_constant,d)*((wide)1<<20);
#endif
        for (unsigned order = 0; order < c->mmr_order; ++order)
            for (unsigned k = 0; k < 7; ++k) v +=
#ifdef DV_PREPARED_CHROMA
                (wide)coefficients[order][k]*(wide)t[order][k];
#else
                coeff(c->mmr[order][k],d)*t[order][k];
#endif
#ifdef DV_BOUNDED_MMR64
        }
#endif
        v = floor_div(v,d+4);
        if (v < 0) v = 0;
        if (v > 65535) v = 65535;
#ifdef DV_PREPARED_CHROMA
        wide r=residual_table[el[i]];
#else
        wide r = 0;
        int q = (int)el[i]-(int)c->nlq_offset;
        if (cfg->residual_enabled && q) {
            int sign = q < 0 ? -1 : 1;
            wide dq = (2*q-sign)*coeff(c->nlq[0],d)+2*sign*coeff(c->nlq[2],d);
            wide limit = 2*coeff(c->nlq[1],d);
            if (dq > limit) dq = limit;
            if (dq < -limit) dq = -limit;
            r = floor_div(dq,d-15);
        }
#endif
        wide code = floor_div(v+r+8,4);
        if (code < 0) code = 0;
        if (code > 4095) code = 4095;
        mapped[i] = (uint16_t)v;
        residual[i] = (int32_t)r;
        sum[i] = (int32_t)(v+r);
        output[i] = (uint16_t)code;
    }
    return 0;
}
