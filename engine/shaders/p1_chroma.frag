#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
#extension GL_GOOGLE_include_directive : require
// Pass 1, one fragment per 4:2:0 chroma sample (cx, k), cw x ch:
//   - the reconstructed chroma (12-bit Cb, Cr): BL chroma phase, the luma
//     guide, the EL chroma (phase, 64/192 vertical, bilinear or Annex B's
//     8-tap horizontal) and
//     the chroma mapping (polynomial table or MMR) plus the NLQ residual, as
//     yblod's luma_phase_guide + phase_vertical_uv + mmr_dual_horizontal /
//     fel_mmr_single / table_add compute them;
//   - the EL luma code at (cx, k) (the EL luma is cw x ch), for pass 2.
// V3D runs this out of texture fetches: neighbouring SAND samples share
// their 32-bit word, and a dual MMR fetches Cb's and Cr's tables at once.
#include "common.glsl"
#include "piecewise.glsl"
#include "el_chroma.glsl"

layout(binding = 3) uniform usamplerBuffer bl_y;
layout(binding = 5) uniform usamplerBuffer el_y;
layout(binding = 10) uniform usamplerBuffer tables128;   // the tables as RGBA32UI
layout(binding = 12) uniform usampler2D el_cv;          // P0: the EL chroma's vertical filter (ELUP != 0)
layout(location = 0) out uvec2 o_chroma;
layout(location = 1) out uint o_el;

// The chroma MMR (pi_engine.h enum pi_mmr_mode): 0 float; 2 exact in emulated
// 64-bit (yblod's); 3 exact, the low word in integers and the high word from
// a float sum (mmr_split); 4 exact, float with a per-sample error bound and
// the split where the bound could change the 12-bit result.
layout(constant_id = 0) const uint MMR_MODE = 4;
// The stream's selectors (pi_engine.c makes a pipeline per combination):
// chroma phase method (METHOD, el_chroma.glsl); dual MMR; chroma kind (0
// polynomial, 1 MMR128, 2 MMR96, 3 MMR64 with LUTs) and MMR order per channel.
layout(constant_id = 2) const uint DUAL = 1;
layout(constant_id = 3) const uint KIND0 = 3;
layout(constant_id = 4) const uint KIND1 = 3;
layout(constant_id = 5) const uint ORDER0 = 3;
layout(constant_id = 6) const uint ORDER1 = 3;
const uint ORDER = ORDER0 > ORDER1 ? ORDER0 : ORDER1;
// MMR curves beyond the 64-bit LUT (MMR96, MMR128): exact in emulated
// 128-bit (mmr_wide) in every exact mode.
const bool WIDE = KIND0 == 1u || KIND0 == 2u || KIND1 == 1u || KIND1 == 2u;
// The single-layer polynomial route (yblod's single_polynomial): the BL's
// own chroma through 12-bit tables, no EL.
layout(constant_id = 7) const uint SINGLE = 0;
// The single-layer general mapping (route 3, yblod's piecewise): 1 its 4:2:0
// layout (ENTRY 0), 2 its full-height 4:2:2 layout (ENTRY 1: both output
// rows' chroma, packed 4 x 12 bits into the two targets); ORDER2 its
// order-2 chroma build (layout 2).
layout(constant_id = 8) const uint PIECE = 0;
layout(constant_id = 9) const uint ORDER2 = 0;
// Route 3 with one piece per chroma channel: pi_engine.c piece_params() has
// written it as an MMR, computed by mmr() in the stream's mode.
layout(constant_id = 10) const uint PIECE_FAST = 0;
// Measurement only (pi_options.measure PI_MEASURE_MARK): where mode 4 falls
// back it writes Q16 65535 / 0 (12-bit 4095 / 0), where the split's guard
// does, 0 / 65535.
layout(constant_id = 11) const uint MARK = 0;
// The split's per-sample guard (modes 3, 4): pi_engine.c's bound for the
// frame's largest magnitudes fails, so the split checks each sample's own
// and takes emulated 64-bit where that fails too.
layout(constant_id = 12) const uint SPLIT_GUARD = 0;
// The EL upscale: ELUP (el_chroma.glsl). For ELUP != 0, pass 0 (p0_elv.frag)
// has filtered the EL chroma vertically into el_cv.
// Measurement only (pi_options.measure PI_MEASURE_EL_OUT): the dual route
// writes the upscaled EL chroma as its chroma.
layout(constant_id = 14) const uint EL_OUT = 0;


// P0's vertical result at EL chroma column x (clamped to the plane), row k.
ivec2 evc(int x, uint k)
{
    return ivec2(texelFetch(el_cv, ivec2(clamp(x, 0, int(P.el_size.x) - 1), int(k)), 0).xy);
}

// The horizontal filter at the odd chroma column between EL chroma columns
// ex and ex + 1, over P0's columns: Annex B's 22 94 -524 2456 2456 -524 94 22
// / 4096 (ex-3 .. ex+4), bicubic's -1 9 9 -1 / 16 (ex-1 .. ex+2) or
// Lanczos-3's 25 -139 626 626 -139 25 / 1024 (ex-2 .. ex+3), clamped to
// 0 .. 1023 (dv_annex_b_scale2x).
uvec2 el_horizontal(uint ex, uint k)
{
    int s = int(ex);
    ivec2 v;
    if (ELUP == 1u) {
        ivec2 sum = 22 * (evc(s - 3, k) + evc(s + 4, k)) + 94 * (evc(s - 2, k) + evc(s + 3, k)) -
                    524 * (evc(s - 1, k) + evc(s + 2, k)) + 2456 * (evc(s, k) + evc(s + 1, k));
        v = (sum + 2048) >> 12;
    } else if (ELUP == 2u) {
        ivec2 sum = -evc(s - 1, k) + 9 * (evc(s, k) + evc(s + 1, k)) - evc(s + 2, k);
        v = (sum + 8) >> 4;
    } else {
        ivec2 sum = 25 * (evc(s - 2, k) + evc(s + 3, k)) - 139 * (evc(s - 1, k) + evc(s + 2, k)) +
                    626 * (evc(s, k) + evc(s + 1, k));
        v = (sum + 512) >> 10;
    }
    return uvec2(clamp(v, ivec2(0), ivec2(1023)));
}

// ---- MMR -------------------------------------------------------------------
// The value: C + LUT_x[s.x] + LUT_y[s.y] + LUT_z[s.z] + the cross features
// k = 3..6 (xy, xz, yz, xyz >> 10) to order 1..3, each power the previous
// times the feature >> 20; then >> (shift + 4), clamped to 0..65535 (Q16).
// Both channels at once; a channel's coefficients beyond its order are 0.
// Written without loops (V3D's compiler leaves loops rolled in a shader this
// size): one term function per cross feature, its indices literal.

// (a * b) >> 20 for a, b < 2^20: the low word by a 32-bit multiply, the high
// word from the float product (within 2^16 of a * b < 2^40: exact).
uint fprod_split(uint a, uint b)
{
    uint lo = a * b;
    uint hi = uint(round((float(a) * float(b) - float(lo)) * (1.0 / 4294967296.0)));
    return (hi << 12) | (lo >> 20);
}
// The same by a 64-bit multiply (yblod's).
uint fprod64(uint a, uint b)
{
    uint uh, ul;
    umulExtended(a, b, uh, ul);
    return (ul >> 20) | (uh << 12);
}

// min(v >> shift, 65535), 0 for v < 0. One return: V3D's compiler turns
// returns inside branches into loops it may leave rolled.
uint map16(uvec2 v, uint shift)
{
    uint r = shift >= 32u ? v.y >> (shift - 32u)
           : (v.y >> shift) != 0u ? 65535u : (v.x >> shift) | (v.y << (32u - shift));
    return int(v.y) < 0 ? 0u : min(r, 65535u);
}

// The univariate tables, Cb and Cr interleaved (one fetch for both).
vec2 lutf(uint t, uint i) { return uintBitsToFloat(texelFetch(tables64, T_LUTF / 2 + int(t * 1024u + i)).rg); }
uvec4 lut64(uint t, uint i) { return texelFetch(tables128, T_LUT64 / 4 + int(t * 1024u + i)); }

// One table fetch of the float MMR (Cb's, Cr's entries; both: one fetch).
vec2 float_lut(uint t, uint i0, uint i1, bool both)
{
    vec2 a = lutf(t, i0);
    return both ? a : vec2(a.x, lutf(t, i1).y);
}

// One cross feature's powers of the float MMR: exact powers (mode 4) or
// float floors (mode 0); adds the terms to v and their magnitudes to mag.
void float_term(uvec2 fa, bool both, bool exact, vec2 c1, vec2 c2, vec2 c3, inout vec2 v, inout vec2 mag)
{
    vec2 a = vec2(fa), t1 = c1 * a;
    v += t1;
    mag += abs(t1);
    if (ORDER > 1u) {
        vec2 b = exact ? vec2(both ? uvec2(fprod_split(fa.x, fa.x)) : uvec2(fprod_split(fa.x, fa.x), fprod_split(fa.y, fa.y)))
                       : floor(a * a * (1.0 / 1048576.0));
        vec2 t2 = c2 * b;
        v += t2;
        mag += abs(t2);
        if (ORDER > 2u) {
            vec2 c = exact ? vec2(both ? uvec2(fprod_split(fa.x, uint(b.x)))
                                       : uvec2(fprod_split(fa.x, uint(b.x)), fprod_split(fa.y, uint(b.y))))
                           : floor(a * b * (1.0 / 1048576.0));
            vec2 t3 = c3 * c;
            v += t3;
            mag += abs(t3);
        }
    }
}

#define FLOAT_TERM(fa, K, EXACT) float_term(fa, both, EXACT, vec2(P.cf[0][K], P.cf[3][K]), \
                                            vec2(P.cf[1][K], P.cf[4][K]), vec2(P.cf[2][K], P.cf[5][K]), v, mag)

// The float MMR of both channels (Q16, unclamped), s0, s1 its inputs (both:
// equal, a dual MMR); exact: the feature powers exact (mode 4), and mag the
// sum of the terms' magnitudes.
vec2 mmr_float_mag(uvec3 s0, uvec3 s1, bool both, bool exact, out vec2 mag)
{
    uint xy0 = s0.x * s0.y, xy1 = s1.x * s1.y;
    vec2 v = P.cconst.xy;
    mag = abs(v);
    vec2 l0 = float_lut(0u, s0.x, s1.x, both), l1 = float_lut(1u, s0.y, s1.y, both), l2 = float_lut(2u, s0.z, s1.z, both);
    v += l0 + l1 + l2;
    mag += abs(l0) + abs(l1) + abs(l2);
    FLOAT_TERM(uvec2(xy0, xy1), 0, exact);
    FLOAT_TERM(uvec2(s0.x * s0.z, s1.x * s1.z), 1, exact);
    FLOAT_TERM(uvec2(s0.y * s0.z, s1.y * s1.z), 2, exact);
    FLOAT_TERM(uvec2((xy0 * s0.z) >> 10, (xy1 * s1.z) >> 10), 3, exact);
    return v;
}

// Mode 2, yblod's emulated 64-bit (Q16, clamped). Loops, against the rule
// above: unrolled, it spilled heavily (92 spills against 6) in the split's
// guarded variant, its only other caller.
uvec2 mmr_exact64(uvec3 s0, uvec3 s1, bool both)
{
    uint xy0 = s0.x * s0.y, xy1 = s1.x * s1.y;
    uvec4 f0 = uvec4(xy0, s0.x * s0.z, s0.y * s0.z, (xy0 * s0.z) >> 10);
    uvec4 f1 = uvec4(xy1, s1.x * s1.z, s1.y * s1.z, (xy1 * s1.z) >> 10);
    uvec2 v0 = P.const64[0].xy, v1 = P.const64[1].xy;
    for (uint t = 0u; t < 3u; ++t) {
        uvec4 q0 = lut64(t, s0[t]), q1 = both ? q0 : lut64(t, s1[t]);
        v0 = add64(v0, q0.xy);
        v1 = add64(v1, q1.zw);
    }
    for (uint k = 0u; k < 4u; ++k) {
        uvec2 a = uvec2(f0[k], f1[k]), b = uvec2(0u), c = uvec2(0u);
        if (ORDER > 1u)
            b = both ? uvec2(fprod64(a.x, a.x)) : uvec2(fprod64(a.x, a.x), fprod64(a.y, a.y));
        if (ORDER > 2u)
            c = both ? uvec2(fprod64(a.x, b.x)) : uvec2(fprod64(a.x, b.x), fprod64(a.y, b.y));
        for (uint o = 0u; o < ORDER; ++o) {
            uvec2 f = o == 0u ? a : o == 1u ? b : c;
            int hi, lo;
            imulExtended(P.c32[o][k], int(f.x), hi, lo);
            v0 = add64(v0, uvec2(lo, hi));
            imulExtended(P.c32[3u + o][k], int(f.y), hi, lo);
            v1 = add64(v1, uvec2(lo, hi));
        }
    }
    return uvec2(map16(v0, P.pk.y + 4u), map16(v1, P.pk.z + 4u));
}

// Mode 3, exact at near float cost: the exact 64-bit sum S's low 32 bits by
// 32-bit integer arithmetic (it wraps: no high multiplies), its high 32 bits
// from float: y, S / 2^32 summed in float, is within 0.5 of the exact value
// (pi_engine.c checks that bound each frame for the largest magnitudes;
// where it fails, SPLIT_GUARD checks each sample's own: split_guarded), so
// H = round(y - L / 2^32) is exact.

// One table fetch (Cb's and Cr's entries) of the split: low words, float
// sum, the terms' magnitudes (mag: unused, so removed, without the guard).
void split_lut(uvec4 q0, uvec4 q1, inout uvec2 lo, inout vec2 y, inout vec2 mag)
{
    const float W = 1.0 / 4294967296.0;
    lo += uvec2(q0.x, q1.z);
    vec2 t = vec2(float(int(q0.y)) + float(q0.x) * W, float(int(q1.w)) + float(q1.z) * W);
    y += t;
    mag += abs(t);
}

// One cross feature's powers to ORDER of the split, coefficients c (the low
// words, order 1..3) and f (/ 2^32), channel 0 then 1.
void split_term(uvec2 fa, bool both, ivec2 c1, ivec2 c2, ivec2 c3, vec2 f1, vec2 f2, vec2 f3, inout uvec2 lo,
                inout vec2 y, inout vec2 mag)
{
    lo += uvec2(c1) * fa;
    vec2 t = f1 * vec2(fa);
    y += t;
    mag += abs(t);
    if (ORDER > 1u) {
        uvec2 fb = both ? uvec2(fprod_split(fa.x, fa.x)) : uvec2(fprod_split(fa.x, fa.x), fprod_split(fa.y, fa.y));
        lo += uvec2(c2) * fb;
        t = f2 * vec2(fb);
        y += t;
        mag += abs(t);
        if (ORDER > 2u) {
            uvec2 fc = both ? uvec2(fprod_split(fa.x, fb.x)) : uvec2(fprod_split(fa.x, fb.x), fprod_split(fa.y, fb.y));
            lo += uvec2(c3) * fc;
            t = f3 * vec2(fc);
            y += t;
            mag += abs(t);
        }
    }
}

#define SPLIT_TERM(fa, K) split_term(fa, both, ivec2(P.c32[0][K], P.c32[3][K]), ivec2(P.c32[1][K], P.c32[4][K]), \
                                     ivec2(P.c32[2][K], P.c32[5][K]), vec2(P.cs[0][K], P.cs[3][K]), \
                                     vec2(P.cs[1][K], P.cs[4][K]), vec2(P.cs[2][K], P.cs[5][K]), lo, y, mag)

// The split MMR; mag: the sum of its terms' magnitudes (/ 2^32).
uvec2 mmr_split_mag(uvec3 s0, uvec3 s1, bool both, out vec2 mag)
{
    uint xy0 = s0.x * s0.y, xy1 = s1.x * s1.y;
    uvec2 lo = uvec2(P.const64[0].x, P.const64[1].x);
    vec2 y = P.cy.xy;
    mag = abs(y);
    uvec4 qx = lut64(0u, s0.x), qy = lut64(1u, s0.y), qz = lut64(2u, s0.z);
    split_lut(qx, both ? qx : lut64(0u, s1.x), lo, y, mag);
    split_lut(qy, both ? qy : lut64(1u, s1.y), lo, y, mag);
    split_lut(qz, both ? qz : lut64(2u, s1.z), lo, y, mag);
    SPLIT_TERM(uvec2(xy0, xy1), 0);
    SPLIT_TERM(uvec2(s0.x * s0.z, s1.x * s1.z), 1);
    SPLIT_TERM(uvec2(s0.y * s0.z, s1.y * s1.z), 2);
    SPLIT_TERM(uvec2((xy0 * s0.z) >> 10, (xy1 * s1.z) >> 10), 3);
    uvec2 hi = uvec2(ivec2(round(y - vec2(lo) * (1.0 / 4294967296.0))));
    return uvec2(map16(uvec2(lo.x, hi.x), P.pk.y + 4u), map16(uvec2(lo.y, hi.y), P.pk.z + 4u));
}

// The split, exact: with SPLIT_GUARD, each sample whose float sum may be 0.5
// off (32 x 2^-24 x its terms' magnitudes; pi_engine.c split_ok) in
// emulated 64-bit instead. One return (see map16).
uvec2 split_guarded(uvec3 s0, uvec3 s1, bool both)
{
    vec2 mag;
    uvec2 m = mmr_split_mag(s0, s1, both, mag);
    if (SPLIT_GUARD != 0u && any(greaterThanEqual(mag * P.err.y, vec2(0.5))))
        m = MARK != 0u ? uvec2(0u, 65535u) : mmr_exact64(s0, s1, both);
    return m;
}

// ---- MMR96 / MMR128: exact in emulated 128-bit ------------------------------
// The planner's 128-bit MMR parameters (T_WIDE, pi_engine.c wide_tables):
// per channel the constant, then coefficient (order o, feature k) at
// 1 + 7o + k, each four words, two's complement. Features: the univariate
// s << 10 and its powers, the cross features as above.

uvec4 wide_word(uint c, uint i) { return texelFetch(tables128, T_WIDE / 4 + int(c * 22u + i)); }

// a + b, mod 2^128
uvec4 add128(uvec4 a, uvec4 b)
{
    uint c0, c1, c2, c3;
    uint x = uaddCarry(a.x, b.x, c0);
    uint y = uaddCarry(a.y, b.y, c1);
    uint y2 = uaddCarry(y, c0, c3);
    uint z = uaddCarry(a.z, b.z, c2);
    uint z2 = uaddCarry(z, c1 + c3, c0);
    return uvec4(x, y2, z2, a.w + b.w + c2 + c0);
}

// a * f, mod 2^128, f unsigned
uvec4 mul128(uvec4 a, uint f)
{
    uint h0, l0, h1, l1, h2, l2;
    umulExtended(a.x, f, h0, l0);
    umulExtended(a.y, f, h1, l1);
    umulExtended(a.z, f, h2, l2);
    return add128(uvec4(l0, l1, l2, a.w * f), uvec4(0u, h0, h1, h2));
}

// min(v >> shift, 65535), 0 for v < 0, 4 <= shift < 64.
uint map16_128(uvec4 v, uint shift)
{
    uint r = (v.z | v.w) != 0u || int(v.y) < 0 ? 65535u : map16(v.xy, shift);   /* >= 2^63: clamps */
    return int(v.w) < 0 ? 0u : r;
}

// One feature's powers of channel c: the feature f (order 1), its square and
// cube (fprod64) against coefficients 1 + k, 8 + k, 15 + k.
uvec4 wide_term(uint c, uint order, uint k, uint f)
{
    uvec4 v = mul128(wide_word(c, 1u + k), f);
    if (order > 1u) {
        uint f2 = fprod64(f, f);
        v = add128(v, mul128(wide_word(c, 8u + k), f2));
        if (order > 2u)
            v = add128(v, mul128(wide_word(c, 15u + k), fprod64(f, f2)));
    }
    return v;
}

uint wide_channel(uint c, uint order, uvec3 s)
{
    uint xy = s.x * s.y;
    uvec4 v = wide_word(c, 0u);
    v = add128(v, wide_term(c, order, 0u, s.x << 10));
    v = add128(v, wide_term(c, order, 1u, s.y << 10));
    v = add128(v, wide_term(c, order, 2u, s.z << 10));
    v = add128(v, wide_term(c, order, 3u, xy));
    v = add128(v, wide_term(c, order, 4u, s.x * s.z));
    v = add128(v, wide_term(c, order, 5u, s.y * s.z));
    v = add128(v, wide_term(c, order, 6u, (xy * s.z) >> 10));
    return map16_128(v, (c == 0u ? P.pk.y : P.pk.z) + 4u);
}

uvec2 mmr_wide(uvec3 s0, uvec3 s1)
{
    return uvec2(wide_channel(0u, ORDER0, s0), wide_channel(1u, ORDER1, s1));
}

uvec2 q16(vec2 v) { return uvec2(clamp(floor(v), vec2(0.0), vec2(65535.0))); }

// Mode 4: the float MMR (Q16) with exact feature powers, and a bound on its
// error. Vulkan's float add and multiply are correctly rounded: each term is
// a product of a coefficient rounded once (on the CPU) and an exact feature,
// summed with 16 terms (constant, 3 tables, 12 cross terms), so the sum is
// within (2 + 15) x 2^-24 of the sum of the terms' magnitudes, to first order;
// P.err.x carries 20 x 2^-24.
vec2 mmr_float_bounded(uvec3 s0, uvec3 s1, bool both, out vec2 bound)
{
    vec2 mag, v = mmr_float_mag(s0, s1, both, true, mag);
    bound = mag * P.err.x;
    return v;
}

// The chroma mapping of both channels plus the residual r: 12-bit codes.
uvec2 mmr(uvec3 s0, uvec3 s1, bool both, ivec2 r)
{
    // one path per specialised pipeline, one return (see map16)
    uvec2 m;
    if (MMR_MODE == 0u) {
        vec2 mag;
        m = q16(mmr_float_mag(s0, s1, both, false, mag));
    } else if (WIDE)
        m = mmr_wide(s0, s1);
    else if (MMR_MODE == 3u)
        m = split_guarded(s0, s1, both);
    else if (MMR_MODE == 4u) {
        // float where the bound can't change the 12-bit result, else exact
        vec2 bound, v = mmr_float_bounded(s0, s1, both, bound);
        uvec2 lo = q16(v - bound), hi = q16(v + bound);
        bool sure = uvec2(recon(lo.x, r.x), recon(lo.y, r.y)) == uvec2(recon(hi.x, r.x), recon(hi.y, r.y));
        m = sure ? lo : MARK != 0u ? uvec2(65535u, 0u) : split_guarded(s0, s1, both);
    } else
        m = mmr_exact64(s0, s1, both);
    return uvec2(recon(m.x, r.x), recon(m.y, r.y));
}

// ---- the luma guide and route 3 ----------------------------------------------
// One row's [1 2 1] / 4 of the BL luma at pixels 2cx-1 .. 2cx+1 (the first clamped).
uint guide_row(uint cx, uint y)
{
    uint n = cx != 0u ? 2u * cx - 1u : 0u, w = div3(n), sub = n - 3u * w;
    uint wa = word(bl_y, P.bl.x, P.bl.z, w, y);
    uint wb = sub != 0u ? word(bl_y, P.bl.x, P.bl.z, w + 1u, y) : wa;   /* pixel 2cx + 1 exists */
    uvec3 t = cx != 0u ? uvec3(pick(wa, wb, sub), pick(wa, wb, sub + 1u), pick(wa, wb, sub + 2u))
                       : uvec3(pick(wa, wb, 0u), pick(wa, wb, 0u), pick(wa, wb, 1u));
    return (t.x + 2u * t.y + t.z + 2u) >> 2;
}

// Route 3's chroma (12-bit Cb, Cr) of input (guide, Cb, Cr).
uvec2 reshape_chroma(uint guide, uvec2 chroma)
{
    uvec2 res;
    if (PIECE_FAST != 0u) {
        uvec3 s = clamp(uvec3(guide, chroma), P.lo[0].xyz, P.hi[0].xyz);
        res = mmr(s, s, true, ivec2(0));
    } else
        res = (ORDER2 != 0u ? reshape_order2_chroma(uvec3(guide, chroma)) : reshape_tuple(uvec3(guide, chroma))).yz;
    return res;
}

// Route 3's full-height 4:2:2 layout: the chroma of output row 2k + r (its
// policy's blend of two 4:2:0 rows) and that row's own guide.
uvec2 row422(uint cx, uint k, uint r)
{
    uint ch = P.size.w, row = 2u * k + r, cy = row >> 1, top, bottom, weight;
    if (P.pw.x == 1u) {
        top = cy;
        bottom = min(cy + 1u, ch - 1u);
        weight = r != 0u ? 2u : 4u;
    } else if (r != 0u) {
        top = cy;
        bottom = min(cy + 1u, ch - 1u);
        weight = 3u;
    } else {
        top = cy != 0u ? cy - 1u : 0u;
        bottom = cy;
        weight = 1u;
    }
    uvec2 chroma = (weight * bl_chroma(cx, top) + (4u - weight) * bl_chroma(cx, bottom) + 2u) >> 2;
    return reshape_chroma(guide_row(cx, row), chroma);
}

void main()
{
    uint cx = uint(gl_FragCoord.x), k = uint(gl_FragCoord.y);
    if (SINGLE != 0u) {
        uvec2 c = min(bl_chroma(cx, k), uvec2(1023u));
        o_chroma = uvec2(texelFetch(tables, T_SINGLE + 1024 + int(c.x)).r, texelFetch(tables, T_SINGLE + 2048 + int(c.y)).r);
        o_el = 0u;
    } else if (PIECE == 2u) {
        // 48 bits: Cb, Cr of row 2k, then of row 2k+1, 12 bits each
        uvec2 a = row422(cx, k, 0u), b = row422(cx, k, 1u);
        o_chroma = uvec2(a.x | ((a.y & 15u) << 12), (a.y >> 4) | ((b.x & 255u) << 8));
        o_el = (b.x >> 8) | (b.y << 4);
    } else {
        o_el = PIECE != 0u ? 0u : sand1(el_y, P.el.x, P.el.z, cx, k);

        // the luma guide: [1 2 1] / 4 on each of the block's rows (pixels
        // 2cx-1 .. 2cx+1, the first clamped), rows averaged
        uint guide = (guide_row(cx, 2u * k) + guide_row(cx, 2u * k + 1u) + 1u) >> 1;
        if (PIECE == 1u)   // the 4:2:0 layout: the decoded chroma and the guide
            o_chroma = reshape_chroma(guide, min(bl_chroma(cx, k), uvec2(1023u)));
        else {
            uvec2 bp = METHOD == 2u ? min(bl_chroma(cx, k), uvec2(1023u)) : phase(false, cx, int(k), P.size.w).xy;

            // the EL chroma at (cx, k): co-sited horizontally, bilinear (or
            // Annex B's 8-tap) between
            uvec2 e;
            if (ELUP != 0u && (cx & 1u) != 0u)
                e = el_horizontal(cx >> 1, k);
            else if (ELUP != 0u)
                e = uvec2(min(evc(int(cx >> 1), k), ivec2(1023)));
            else {
                uvec4 ev = el_vertical(cx >> 1, k);
                e = min((cx & 1u) != 0u ? (ev.xy + ev.zw + 1u) >> 1 : ev.xy, uvec2(1023u));
            }

            ivec2 r = ivec2(int(texelFetch(tables, T_RES + 1024 + int(e.x)).r), int(texelFetch(tables, T_RES + 2048 + int(e.y)).r));
            uvec3 s = uvec3(guide, bp.x, bp.y);
            if (EL_OUT != 0u)
                o_chroma = e;
            else if (KIND0 != 0u && KIND1 != 0u) {
                if (DUAL != 0u) {   // both channels clamp the input by channel 0's bounds
                    uvec3 c = clamp(s, P.lo[0].xyz, P.hi[0].xyz);
                    o_chroma = mmr(c, c, true, r);
                } else
                    o_chroma = mmr(clamp(s, P.lo[0].xyz, P.hi[0].xyz), clamp(s, P.lo[1].xyz, P.hi[1].xyz), false, r);
            } else {
                // polynomial channels: the chroma mapping table (an MMR one: its own)
                uvec2 mm = (KIND0 != 0u || KIND1 != 0u)
                               ? mmr(clamp(s, P.lo[0].xyz, P.hi[0].xyz), clamp(s, P.lo[1].xyz, P.hi[1].xyz), false, r)
                               : uvec2(0u);
                o_chroma = uvec2(KIND0 == 0u ? recon(texelFetch(tables, T_CMAP + int(bp.x)).r, r.x) : mm.x,
                                 KIND1 == 0u ? recon(texelFetch(tables, T_CMAP + 1024 + int(bp.y)).r, r.y) : mm.y);
            }
        }
    }
}
