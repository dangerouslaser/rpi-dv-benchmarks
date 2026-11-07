/* Private scalar reference for ETSI GS CCM 001 v1.1.1, 5.4.2/5.4.3.
 * No scaling, colour transform, or hardware-equivalence claim.
 * Restricted initial contract: BL/EL 10 bits, reconstructed output 12 bits.
 */
#include "pixel_polynomial.h"
#include <stdint.h>
#include <string.h>

typedef __int128 wide;
static wide floor_shift(wide x, unsigned n)
{
    const wide d = (wide)1 << n;
    return x >= 0 ? x / d : -((-x + d - 1) / d);
}
static wide coefficient(dv_exact_coefficient c, unsigned d)
{
    return (wide)c.integer * ((wide)1 << d) + c.fraction;
}

/* Outputs: mapped unsigned Q16, signed residual Q16, final 12-bit code.
 * The upper pivot endpoint deliberately uses the last valid piece.
 * Invalid requests leave all outputs unchanged.
 */
int dv_reference_polynomial(const dv_intel_composer_config *cfg, unsigned component, uint16_t bl,
                      uint16_t el, int32_t *mapped, int32_t *residual,
                      uint16_t *output)
{
    if (!cfg || component > 2 || !mapped || !residual || !output || cfg->abi_version != 1 ||
        cfg->base_depth != 10 || cfg->enhancement_depth != 10 ||
        cfg->reconstruction_depth != 12 || cfg->residual_enabled > 1 ||
        (cfg->residual_enabled && cfg->coefficient_log2_denominator < 15) ||
        cfg->coefficient_log2_denominator > 32 || bl > 1023 || el > 1023)
        return -1;
    const dv_component_config *c = &cfg->component[component];
    unsigned d = cfg->coefficient_log2_denominator;
    if (c->mapping || c->pivot_count < 2 || c->pivot_count > 9 ||
        c->nlq_offset > 1023) return -1;
    for (unsigned i = 0; i < c->pivot_count; ++i)
        if (c->pivots[i] > 1023 || (i && c->pivots[i] <= c->pivots[i-1]))
            return -1;
    for (unsigned i = 0; i + 1 < c->pivot_count; ++i) {
        if (c->polynomial_order[i] < 1 || c->polynomial_order[i] > 2) return -1;
        for (unsigned k = 0; k <= c->polynomial_order[i]; ++k)
            if ((uint64_t)c->polynomial[i][k].fraction >= (UINT64_C(1) << d))
                return -1;
    }
    for (unsigned i = 0; i < 3; ++i)
        if ((uint64_t)c->nlq[i].fraction >= (UINT64_C(1) << d) ||
            coefficient(c->nlq[i], d) < 0) return -1;
    unsigned piece = c->pivot_count - 2;
    for (unsigned i = 0; i + 1 < c->pivot_count; ++i)
        if (bl < c->pivots[i+1]) { piece = i; break; }
    unsigned s = bl;
    if (s < c->pivots[0]) s = c->pivots[0];
    if (s > c->pivots[c->pivot_count-1]) s = c->pivots[c->pivot_count-1];
    wide vv = 0, power = 1;
    for (unsigned i = 0; i <= c->polynomial_order[piece]; ++i) {
        vv += coefficient(c->polynomial[piece][i], d) * power *
              ((wide)1 << (20 - 10*i));
        power *= s;
    }
    wide v = vv < 0 ? 0 : floor_shift(vv, 4+d);
    if (v > 65535) v = 65535;
    wide r = 0;
    int q = (int)el - (int)c->nlq_offset;
    if (cfg->residual_enabled && q) {
        int sign = q < 0 ? -1 : 1;
        wide dq = (2*q-sign) * coefficient(c->nlq[0], d) +
                  2*sign * coefficient(c->nlq[2], d);
        wide limit = 2 * coefficient(c->nlq[1], d);
        if (dq > limit) dq = limit;
        if (dq < -limit) dq = -limit;
        r = floor_shift(dq, d-15);
        if (r < INT32_MIN || r > INT32_MAX) return -1;
    }
    wide code = floor_shift(v+r+8, 4);
    if (code < 0) code = 0;
    if (code > 4095) code = 4095;
    *mapped = (int32_t)v;
    *residual = (int32_t)r;
    *output = (uint16_t)code;
    return 0;
}

int dv_reference_luma(const dv_intel_composer_config *cfg, uint16_t bl,
                      uint16_t el, int32_t *mapped, int32_t *residual,
                      uint16_t *output)
{
    return dv_reference_polynomial(cfg,0,bl,el,mapped,residual,output);
}

int dv_prepare_polynomial_tables(const dv_intel_composer_config *cfg,uint16_t table[3][1024])
{
    if(!cfg||!table||cfg->residual_enabled)return -1;
    uint16_t result[3][1024];int32_t mapped,residual;
    for(unsigned c=0;c<3;++c)for(unsigned code=0;code<1024;++code)
        if(dv_reference_polynomial(cfg,c,(uint16_t)code,0,&mapped,&residual,&result[c][code]))return -1;
    memcpy(table,result,sizeof(result));return 0;
}

int dv_prepare_polynomial_component(const dv_intel_composer_config *cfg,unsigned c,
                                    uint16_t mapped_table[1024],int32_t residual_table[1024])
{
    if(!cfg||c>2||!mapped_table||!residual_table)return -1;
    uint16_t mapping[1024],code;int32_t residuals[1024],mapped,residual;
    for(unsigned i=0;i<1024;++i){
        if(dv_reference_polynomial(cfg,c,(uint16_t)i,(uint16_t)cfg->component[c].nlq_offset,
                                   &mapped,&residual,&code))return -1;
        mapping[i]=(uint16_t)mapped;
        if(dv_reference_polynomial(cfg,c,0,(uint16_t)i,&mapped,&residuals[i],&code))return -1;
    }
    memcpy(mapped_table,mapping,sizeof(mapping));
    memcpy(residual_table,residuals,sizeof(residuals));return 0;
}
