#version 450
#extension GL_GOOGLE_include_directive : require
// Pass 1, one fragment per 4:2:0 chroma sample (cx, k), cw x ch:
//   - the reconstructed chroma (12-bit Cb, Cr): BL chroma phase, the luma
//     guide, the EL chroma (phase, 64/192 vertical, bilinear horizontal) and
//     the chroma mapping (polynomial table or MMR) plus the NLQ residual, as
//     yblod's luma_phase_guide + phase_vertical_uv + mmr_dual_horizontal /
//     fel_mmr_single / table_add compute them;
//   - the EL luma code at (cx, k) (the EL luma is cw x ch), for pass 3.
// V3D runs this out of texture fetches: neighbouring SAND samples share
// their 32-bit word, and a dual MMR fetches Cb's and Cr's tables at once.
#include "common.glsl"
#include "piecewise.glsl"

layout(binding = 3) uniform usamplerBuffer bl_y;
layout(binding = 4) uniform usamplerBuffer bl_c;
layout(binding = 5) uniform usamplerBuffer el_y;
layout(binding = 6) uniform usamplerBuffer el_c;
layout(binding = 10) uniform usamplerBuffer tables128;   // the tables as RGBA32UI
layout(location = 0) out uvec2 o_chroma;
layout(location = 1) out uint o_el;

// 0 float, 1 exact in 32-bit pieces, 2 exact in emulated 64-bit (yblod's),
// 3 exact: the low word in integers, the high word from float (mmr_split)
layout(constant_id = 0) const uint MMR_MODE = 1;
// The stream's selectors (pi_engine.c makes a pipeline per combination):
// chroma phase method; dual MMR; chroma kind (0 polynomial, else MMR) and
// MMR order per channel.
layout(constant_id = 1) const uint METHOD = 2;
layout(constant_id = 2) const uint DUAL = 1;
layout(constant_id = 3) const uint KIND0 = 3;
layout(constant_id = 4) const uint KIND1 = 3;
layout(constant_id = 5) const uint ORDER0 = 3;
layout(constant_id = 6) const uint ORDER1 = 3;
const uint ORDER = ORDER0 > ORDER1 ? ORDER0 : ORDER1;
// The single-layer polynomial route (yblod's single_polynomial): the BL's
// own chroma through 12-bit tables, no EL.
layout(constant_id = 7) const uint SINGLE = 0;
// The single-layer general mapping (route 3, yblod's piecewise): 1 its 4:2:0
// layout (ENTRY 0), 2 its full-height 4:2:2 layout (ENTRY 1: both output
// rows' chroma, packed 4 x 12 bits into the two targets); ORDER2 its
// order-2 chroma build (layout 2).
layout(constant_id = 8) const uint PIECE = 0;
layout(constant_id = 9) const uint ORDER2 = 0;
// Route 3 with one piece per chroma channel: pi_engine.c piece_fast() has
// written it as an MMR, computed by mmr() in the stream's exact mode.
layout(constant_id = 10) const uint PIECE_FAST = 0;
// Measurement only (PI_MARK): mode 4 writes 4095 / 0 where it falls back,
// the split's guard 0 / 4095.
layout(constant_id = 11) const uint MARK = 0;
// The split's per-sample guard (modes 3, 4): pi_engine.c's bound for the
// frame's largest magnitudes fails, so mmr_split checks each sample's own
// and takes emulated 64-bit where that fails too.
layout(constant_id = 12) const uint SPLIT_GUARD = 0;


// ---- SAND reads ------------------------------------------------------------
uint div3(uint n) { return uint(float(n) * 0.33333334 + 0.1); }
uint word(usamplerBuffer buf, uint base, uint colh, uint w, uint row)
{
    uint b = w * 4u;
    return texelFetch(buf, int(base + ((b & 127u) >> 2) + row * 32u + (b >> 7) * 32u * colh)).r;
}
// sample j (0..5) of the word pair a, b
uint pick(uint a, uint b, uint j) { return ((j < 3u ? a : b) >> (10u * (j < 3u ? j : j - 3u))) & 1023u; }
// samples n, n+1 (one word unless n is a word's last)
uvec2 sand2(usamplerBuffer buf, uint base, uint colh, uint n, uint row)
{
    uint w = div3(n), sub = n - 3u * w, a = word(buf, base, colh, w, row);
    uint b = sub == 2u ? word(buf, base, colh, w + 1u, row) : a;
    return uvec2(pick(a, b, sub), pick(a, b, sub + 1u));
}
// samples n .. n+3 (two words)
uvec4 sand4(usamplerBuffer buf, uint base, uint colh, uint n, uint row)
{
    uint w = div3(n), sub = n - 3u * w;
    uint a = word(buf, base, colh, w, row), b = word(buf, base, colh, w + 1u, row);
    return uvec4(pick(a, b, sub), pick(a, b, sub + 1u), pick(a, b, sub + 2u), pick(a, b, sub + 3u));
}

// (Cb, Cr) of chroma column x and of x + 1 (clamped to the plane)
uvec4 uvq(bool enh, uint x, uint row)
{
    uint cw = enh ? P.el_size.x : P.size.z;
    uvec4 q = enh ? sand4(el_c, P.el.y, P.el.w, 2u * x, row) : sand4(bl_c, P.bl.y, P.bl.w, 2u * x, row);
    return x + 1u < cw ? q : q.xyxy;
}

// phase_uv_sample of columns x and x + 1: 2 none (the decoded chroma), 0 the
// 2-tap, 1 the 4-tap.
uvec4 phase(bool enh, uint x, int row, uint h)
{
    // one return (V3D's compiler turns returns inside branches into loops)
    uint y = uint(edge(row, h));
    uvec4 res;
    if (METHOD == 2u)
        res = min(uvq(enh, x, y), uvec4(1023u));
    else if (METHOD == 0u) {
        ivec4 v = 3 * ivec4(uvq(enh, x, y)) + ivec4(uvq(enh, x, uint(edge(int(y) + 1, h))));
        res = min(uvec4(max(v + 2, ivec4(0)) >> 2), uvec4(1023u));
    } else {
        ivec4 v = -9 * ivec4(uvq(enh, x, uint(edge(int(y) - 1, h)))) + 111 * ivec4(uvq(enh, x, y)) +
                  29 * ivec4(uvq(enh, x, uint(edge(int(y) + 1, h)))) - 3 * ivec4(uvq(enh, x, uint(edge(int(y) + 2, h))));
        res = min(uvec4(max(v + 64, ivec4(0)) >> 7), uvec4(1023u));
    }
    return res;
}

// phase_vertical_uv at EL chroma columns ex, ex + 1, for chroma row k.
uvec4 el_vertical(uint ex, uint k)
{
    int yy = int(k >> 1);
    uint eh = P.el_size.y;
    bool even = (k & 1u) == 0u;
    uvec4 b = phase(true, ex, yy, eh), n = phase(true, ex, even ? yy - 1 : yy + 1, eh);
    return (192u * b + 64u * n + 128u) >> 8;
}

// ---- MMR -------------------------------------------------------------------
// The value: C + LUT_x[s.x] + LUT_y[s.y] + LUT_z[s.z] + the cross features
// k = 3..6 (xy, xz, yz, xyz >> 10) to order 1..3, each power the previous
// times the feature >> 20; then >> (shift + 4), clamped to 0..65535 (Q16).
// Both channels at once; a channel's coefficients beyond its order are 0.

// (a * b) >> 20 for a, b < 2^20, from 10-bit halves: no high multiply.
uint fprod(uint a, uint b)
{
    uint a1 = a >> 10, a0 = a & 1023u, b1 = b >> 10, b0 = b & 1023u;
    uint low = ((a1 * b0 + a0 * b1) << 10) + a0 * b0;
    return a1 * b1 + (low >> 20);
}
// The same, its low word by a 32-bit multiply and its high word from the
// float product (within 2^16 of a * b < 2^40: the rounding is exact).
uint fprod_split(uint a, uint b)
{
    uint lo = a * b;
    uint hi = uint(round((float(a) * float(b) - float(lo)) * (1.0 / 4294967296.0)));
    return (hi << 12) | (lo >> 20);
}

uint fprod64(uint a, uint b)
{
    uint uh, ul;
    umulExtended(a, b, uh, ul);
    return (ul >> 20) | (uh << 12);
}

uvec2 add64(uvec2 a, uvec2 b)
{
    uint c;
    uint lo = uaddCarry(a.x, b.x, c);
    return uvec2(lo, a.y + b.y + c);
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

// One cross feature's powers of the float MMR: exact powers (EXACT, mode 4)
// or float floors (mode 0); adds the terms to v and their magnitudes to mag.
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
// sum of the terms' magnitudes. No loops, one return (V3D's compiler leaves
// loops rolled in a shader this size).
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

vec2 mmr_float(uvec3 s0, uvec3 s1, bool both)
{
    vec2 mag;
    return mmr_float_mag(s0, s1, both, false, mag);
}

// Mode 3, exact at near float cost: the exact 64-bit sum S's low 32 bits by
// 32-bit integer arithmetic (it wraps: no high multiplies), its high 32 bits
// from float: y, S / 2^32 summed in float, is within 0.5 of the exact value
// (pi_engine.c checks that bound each frame for the largest magnitudes;
// where it fails, SPLIT_GUARD checks each sample's own: split_guarded), so
// H = round(y - L / 2^32) is exact. Features exact (fprod, 10-bit halves).
// Written without loops: V3D's compiler left the looped version's 11 loops
// rolled (at 2 threads).
const float SPLIT_W = 1.0 / 4294967296.0;

// One table fetch (Cb's and Cr's entries) of mmr_split: low words, float
// sum, the terms' magnitudes (mag: unused, so removed, without the guard).
void split_lut(uvec4 q0, uvec4 q1, inout uvec2 lo, inout vec2 y, inout vec2 mag)
{
    const float W = 1.0 / 4294967296.0;
    lo += uvec2(q0.x, q1.z);
    vec2 t = vec2(float(int(q0.y)) + float(q0.x) * W, float(int(q1.w)) + float(q1.z) * W);
    y += t;
    mag += abs(t);
}

// One cross feature's powers to ORDER of mmr_split, coefficients c (the
// low words, order 1..3) and cf (/ 2^32), channel 0 then 1: low words and
// float sum. All indices literal at the call (no dynamic indexing).
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

// The exact MMR of both channels (Q16, clamped): 1 in 32-bit pieces, 2 in
// emulated 64-bit (yblod's).
uvec2 mmr_exact(uvec3 s0, uvec3 s1, bool both, uint mode)
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
    if (mode == 1u) {
        // coefficient (hi * 2^16 + lo) x feature (f1 * 2^10 + f0), four
        // accumulators by weight per channel: products < 2^26, sums < 2^30.
        uvec2 a0 = uvec2(0u), a10 = uvec2(0u);
        ivec2 a16 = ivec2(0), a26 = ivec2(0);
        for (uint k = 0u; k < 4u; ++k) {
            uvec2 fa = uvec2(f0[k], f1[k]), fb = uvec2(0u), fc = uvec2(0u);
            if (ORDER > 1u)
                fb = both ? uvec2(fprod(fa.x, fa.x)) : uvec2(fprod(fa.x, fa.x), fprod(fa.y, fa.y));
            if (ORDER > 2u)
                fc = both ? uvec2(fprod(fa.x, fb.x)) : uvec2(fprod(fa.x, fb.x), fprod(fa.y, fb.y));
            for (uint o = 0u; o < ORDER; ++o) {
                uvec2 f = o == 0u ? fa : o == 1u ? fb : fc;
                uvec2 lo = uvec2(P.c_lo[o][k], P.c_lo[3u + o][k]);
                ivec2 hi = ivec2(P.c_hi[o][k], P.c_hi[3u + o][k]);
                uvec2 lo_f = f & 1023u, hi_f = f >> 10;
                a0 += lo * lo_f;
                a10 += lo * hi_f;
                a16 += hi * ivec2(lo_f);
                a26 += hi * ivec2(hi_f);
            }
        }
        v0 = add64(v0, uvec2(a0.x, 0u));
        v0 = add64(v0, uvec2(a10.x << 10, a10.x >> 22));
        v0 = add64(v0, uvec2(uint(a16.x) << 16, uint(a16.x >> 16)));
        v0 = add64(v0, uvec2(uint(a26.x) << 26, uint(a26.x >> 6)));
        v1 = add64(v1, uvec2(a0.y, 0u));
        v1 = add64(v1, uvec2(a10.y << 10, a10.y >> 22));
        v1 = add64(v1, uvec2(uint(a16.y) << 16, uint(a16.y >> 16)));
        v1 = add64(v1, uvec2(uint(a26.y) << 26, uint(a26.y >> 6)));
    } else {
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
    }
    return uvec2(map16(v0, P.pk.y + 4u), map16(v1, P.pk.z + 4u));
}

// The split MMR, exact: with SPLIT_GUARD, each sample whose float sum may
// be 0.5 off (32 x 2^-24 x its terms' magnitudes; see pi_engine.c split_ok)
// in emulated 64-bit instead. One return (see map16).
uvec2 split_guarded(uvec3 s0, uvec3 s1, bool both)
{
    vec2 mag;
    uvec2 m = mmr_split_mag(s0, s1, both, mag);
    if (SPLIT_GUARD != 0u && any(greaterThanEqual(mag * P.err.y, vec2(0.5))))
        m = MARK != 0u ? uvec2(0u, 65535u) : mmr_exact(s0, s1, both, 2u);
    return m;
}

// One row's [1 2 1] / 4 of the BL luma at pixels 2cx-1 .. 2cx+1 (the first clamped).
uint guide_row(uint cx, uint y)
{
    uint n = cx != 0u ? 2u * cx - 1u : 0u, w = div3(n), sub = n - 3u * w;
    uint wa = word(bl_y, P.bl.x, P.bl.z, w, y);
    uint wb = sub != 0u ? word(bl_y, P.bl.x, P.bl.z, w + 1u, y) : wa;
    uvec3 t = cx != 0u ? uvec3(pick(wa, wb, sub), pick(wa, wb, sub + 1u), pick(wa, wb, sub + 2u))
                       : uvec3(pick(wa, wb, 0u), pick(wa, wb, 0u), pick(wa, wb, 1u));
    return (t.x + 2u * t.y + t.z + 2u) >> 2;
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

uvec2 mmr(uvec3 s0, uvec3 s1, bool both, ivec2 r)
{
    // one path per specialised pipeline, one return (see map16)
    uvec2 m;
    if (MMR_MODE == 3u)
        m = split_guarded(s0, s1, both);
    else if (MMR_MODE == 0u)
        m = q16(mmr_float(s0, s1, both));
    else if (MMR_MODE == 4u) {
        // float where the bound can't change the 12-bit result, else exact
        vec2 bound, v = mmr_float_bounded(s0, s1, both, bound);
        uvec2 lo = q16(v - bound), hi = q16(v + bound);
        bool sure = uvec2(recon(lo.x, r.x), recon(lo.y, r.y)) == uvec2(recon(hi.x, r.x), recon(hi.y, r.y));
        m = sure ? lo : MARK != 0u ? uvec2(65535u, 0u) : split_guarded(s0, s1, both);
    } else
        m = mmr_exact(s0, s1, both, MMR_MODE);
    return uvec2(recon(m.x, r.x), recon(m.y, r.y));
}

uvec3 reshape_chroma(uint guide, uvec2 chroma)
{
    if (PIECE_FAST != 0u) {
        uvec3 s = clamp(uvec3(guide, chroma), P.lo[0].xyz, P.hi[0].xyz);
        return uvec3(0u, mmr(s, s, true, ivec2(0)));
    }
    return ORDER2 != 0u ? reshape_order2_chroma(uvec3(guide, chroma)) : reshape_tuple(uvec3(guide, chroma));
}

void main()
{
    uint cx = uint(gl_FragCoord.x), k = uint(gl_FragCoord.y);
    if (SINGLE != 0u) {
        uvec2 c = min(sand2(bl_c, P.bl.y, P.bl.w, 2u * cx, k), uvec2(1023u));
        o_chroma = uvec2(texelFetch(tables, T_SINGLE + 1024 + int(c.x)).r, texelFetch(tables, T_SINGLE + 2048 + int(c.y)).r);
        o_el = 0u;
    } else if (PIECE == 2u) {
        // per output row: the chroma between two 4:2:0 rows (the policy's
        // weights), the guide of that row alone
        uint ch = P.size.w, o[4];
        for (uint r = 0u; r < 2u; ++r) {
            uint row = 2u * k + r, cy = row >> 1, top, bottom, weight;
            if (P.pw.x == 1u) {
                top = cy;
                bottom = min(cy + 1u, ch - 1u);
                weight = (row & 1u) != 0u ? 2u : 4u;
            } else if ((row & 1u) != 0u) {
                top = cy;
                bottom = min(cy + 1u, ch - 1u);
                weight = 3u;
            } else {
                top = cy != 0u ? cy - 1u : 0u;
                bottom = cy;
                weight = 1u;
            }
            uvec2 first = sand2(bl_c, P.bl.y, P.bl.w, 2u * cx, top), second = sand2(bl_c, P.bl.y, P.bl.w, 2u * cx, bottom);
            uvec2 chroma = (weight * first + (4u - weight) * second + 2u) >> 2;
            uint x = 2u * cx, n = x != 0u ? x - 1u : 0u, w = div3(n), sub = n - 3u * w;
            uint wa = word(bl_y, P.bl.x, P.bl.z, w, row), wb = word(bl_y, P.bl.x, P.bl.z, w + 1u, row);
            uvec3 t = x != 0u ? uvec3(pick(wa, wb, sub), pick(wa, wb, sub + 1u), pick(wa, wb, sub + 2u))
                              : uvec3(pick(wa, wb, 0u), pick(wa, wb, 0u), pick(wa, wb, 1u));
            uint guide = (t.x + 2u * t.y + t.z + 2u) >> 2;
            uvec3 res = reshape_chroma(guide, chroma);
            o[2u * r] = res.y;
            o[2u * r + 1u] = res.z;
        }
        // 48 bits: Cb, Cr of row 2k, then of row 2k+1, 12 bits each
        o_chroma = uvec2(o[0] | ((o[1] & 15u) << 12), (o[1] >> 4) | ((o[2] & 255u) << 8));
        o_el = (o[2] >> 8) | (o[3] << 4);
    } else {
    o_el = PIECE != 0u ? 0u : sand2(el_y, P.el.x, P.el.z, cx, k).x;

    // the luma guide: [1 2 1] / 4 on each of the block's rows (pixels
    // 2cx-1 .. 2cx+1, the first clamped), rows averaged
    uint guide = (guide_row(cx, 2u * k) + guide_row(cx, 2u * k + 1u) + 1u) >> 1;
    if (PIECE == 1u) {   // the 4:2:0 layout: the decoded chroma and the guide
        uvec3 res = reshape_chroma(guide, min(sand2(bl_c, P.bl.y, P.bl.w, 2u * cx, k), uvec2(1023u)));
        o_chroma = res.yz;
    } else {
    uvec2 bp;
    if (METHOD == 2u)
        bp = min(sand2(bl_c, P.bl.y, P.bl.w, 2u * cx, k), uvec2(1023u));
    else
        bp = phase(false, cx, int(k), P.size.w).xy;

    // the EL chroma at (cx, k): co-sited horizontally, bilinear between
    uvec4 ev = el_vertical(cx >> 1, k);
    uvec2 e = min((cx & 1u) != 0u ? (ev.xy + ev.zw + 1u) >> 1 : ev.xy, uvec2(1023u));

    ivec2 r = ivec2(int(texelFetch(tables, T_RES + 1024 + int(e.x)).r), int(texelFetch(tables, T_RES + 2048 + int(e.y)).r));
    uvec3 s = uvec3(guide, bp.x, bp.y);
    if (KIND0 != 0u && KIND1 != 0u) {
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
