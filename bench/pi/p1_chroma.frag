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

layout(binding = 3) uniform usamplerBuffer bl_y;
layout(binding = 4) uniform usamplerBuffer bl_c;
layout(binding = 5) uniform usamplerBuffer el_y;
layout(binding = 6) uniform usamplerBuffer el_c;
layout(binding = 10) uniform usamplerBuffer tables128;   // the tables as RGBA32UI
layout(location = 0) out uvec2 o_chroma;
layout(location = 1) out uint o_el;

// 0 float, 1 exact in 32-bit pieces, 2 exact in emulated 64-bit (yblod's)
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
    uint y = uint(edge(row, h));
    if (METHOD == 2u)
        return min(uvq(enh, x, y), uvec4(1023u));
    ivec4 v;
    if (METHOD == 0u) {
        v = 3 * ivec4(uvq(enh, x, y)) + ivec4(uvq(enh, x, uint(edge(int(y) + 1, h))));
        return min(uvec4(max(v + 2, ivec4(0)) >> 2), uvec4(1023u));
    }
    const int taps[4] = int[4](-9, 111, 29, -3);
    v = ivec4(0);
    for (int k = 0; k < 4; ++k)
        v += taps[k] * ivec4(uvq(enh, x, uint(edge(int(y) + k - 1, h))));
    return min(uvec4(max(v + 64, ivec4(0)) >> 7), uvec4(1023u));
}

// phase_vertical_uv at EL chroma columns ex, ex + 1, for chroma row k.
uvec4 el_vertical(uint ex, uint k)
{
    int yy = int(k >> 1);
    uint eh = P.el_size.y;
    uvec4 b = phase(true, ex, yy, eh);
    if ((k & 1u) == 0u)
        return (64u * phase(true, ex, yy - 1, eh) + 192u * b + 128u) >> 8;
    return (192u * b + 64u * phase(true, ex, yy + 1, eh) + 128u) >> 8;
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

// min(v >> shift, 65535), 0 for v < 0.
uint map16(uvec2 v, uint shift)
{
    if (int(v.y) < 0)
        return 0u;
    if (shift >= 32u)
        return min(v.y >> (shift - 32u), 65535u);
    if ((v.y >> shift) != 0u)
        return 65535u;
    return min((v.x >> shift) | (v.y << (32u - shift)), 65535u);
}

// The univariate tables, Cb and Cr interleaved (one fetch for both).
vec2 lutf(uint t, uint i) { return uintBitsToFloat(texelFetch(tables64, T_LUTF / 2 + int(t * 1024u + i)).rg); }
uvec4 lut64(uint t, uint i) { return texelFetch(tables128, T_LUT64 / 4 + int(t * 1024u + i)); }

// MMR of both channels; s0, s1 their inputs (both: equal, a dual MMR).
uvec2 mmr(uvec3 s0, uvec3 s1, bool both)
{
    uint xy0 = s0.x * s0.y, xy1 = s1.x * s1.y;
    uint f0[4] = uint[4](xy0, s0.x * s0.z, s0.y * s0.z, (xy0 * s0.z) >> 10);
    uint f1[4] = uint[4](xy1, s1.x * s1.z, s1.y * s1.z, (xy1 * s1.z) >> 10);
    if (MMR_MODE == 0u) {
        vec2 v = P.cconst.xy;
        for (uint t = 0u; t < 3u; ++t)
            v += both ? lutf(t, s0[t]) : vec2(lutf(t, s0[t]).x, lutf(t, s1[t]).y);
        for (uint k = 0u; k < 4u; ++k) {
            vec2 a = vec2(float(f0[k]), float(f1[k]));
            v += vec2(P.cf[0][k], P.cf[3][k]) * a;
            if (ORDER > 1u) {
                vec2 b = floor(a * a * (1.0 / 1048576.0));
                v += vec2(P.cf[1][k], P.cf[4][k]) * b;
                if (ORDER > 2u)
                    v += vec2(P.cf[2][k], P.cf[5][k]) * floor(a * b * (1.0 / 1048576.0));
            }
        }
        return uvec2(max(min(v, vec2(65535.0)), vec2(0.0)));
    }
    uvec2 v0 = P.const64[0].xy, v1 = P.const64[1].xy;
    for (uint t = 0u; t < 3u; ++t) {
        uvec4 q0 = lut64(t, s0[t]), q1 = both ? q0 : lut64(t, s1[t]);
        v0 = add64(v0, q0.xy);
        v1 = add64(v1, q1.zw);
    }
    if (MMR_MODE == 1u) {
        // coefficient (hi * 2^16 + lo) x feature (f1 * 2^10 + f0), four
        // accumulators by weight per channel: products < 2^26, sums < 2^30.
        uvec2 a0 = uvec2(0u), a10 = uvec2(0u);
        ivec2 a16 = ivec2(0), a26 = ivec2(0);
        for (uint k = 0u; k < 4u; ++k) {
            uvec2 f[3];
            f[0] = uvec2(f0[k], f1[k]);
            if (ORDER > 1u)
                f[1] = both ? uvec2(fprod(f[0].x, f[0].x)) : uvec2(fprod(f[0].x, f[0].x), fprod(f[0].y, f[0].y));
            if (ORDER > 2u)
                f[2] = both ? uvec2(fprod(f[0].x, f[1].x)) : uvec2(fprod(f[0].x, f[1].x), fprod(f[0].y, f[1].y));
            for (uint o = 0u; o < ORDER; ++o) {
                uvec2 lo = uvec2(P.c_lo[o][k], P.c_lo[3u + o][k]);
                ivec2 hi = ivec2(P.c_hi[o][k], P.c_hi[3u + o][k]);
                uvec2 lo_f = f[o] & 1023u, hi_f = f[o] >> 10;
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
        // MMR_MODE 2: yblod's emulated 64-bit (umulExtended per product)
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

void main()
{
    uint cx = uint(gl_FragCoord.x), k = uint(gl_FragCoord.y);
    o_el = sand2(el_y, P.el.x, P.el.z, cx, k).x;

    // the luma guide: [1 2 1] / 4 on each of the block's rows (pixels
    // 2cx-1 .. 2cx+1, the first clamped), rows averaged
    uint a[2];
    for (uint r = 0u; r < 2u; ++r) {
        uint y = 2u * k + r;
        uvec3 t;
        if (cx == 0u) {
            uvec2 q = sand2(bl_y, P.bl.x, P.bl.z, 0u, y);
            t = uvec3(q.x, q.x, q.y);
        } else {
            uint n = 2u * cx - 1u, w = div3(n), sub = n - 3u * w;
            uint wa = word(bl_y, P.bl.x, P.bl.z, w, y);
            uint wb = sub != 0u ? word(bl_y, P.bl.x, P.bl.z, w + 1u, y) : wa;
            t = uvec3(pick(wa, wb, sub), pick(wa, wb, sub + 1u), pick(wa, wb, sub + 2u));
        }
        a[r] = (t.x + 2u * t.y + t.z + 2u) >> 2;
    }
    uint guide = (a[0] + a[1] + 1u) >> 1;
    uvec2 bp;
    if (METHOD == 2u)
        bp = min(sand2(bl_c, P.bl.y, P.bl.w, 2u * cx, k), uvec2(1023u));
    else
        bp = phase(false, cx, int(k), P.size.w).xy;

    // the EL chroma at (cx, k): co-sited horizontally, bilinear between
    uvec4 ev = el_vertical(cx >> 1, k);
    uvec2 e = min((cx & 1u) != 0u ? (ev.xy + ev.zw + 1u) >> 1 : ev.xy, uvec2(1023u));

    uvec2 m;
    uvec3 s = uvec3(guide, bp.x, bp.y);
    if (KIND0 != 0u && KIND1 != 0u) {
        if (DUAL != 0u) {   // both channels clamp the input by channel 0's bounds
            uvec3 c = clamp(s, P.lo[0].xyz, P.hi[0].xyz);
            m = mmr(c, c, true);
        } else
            m = mmr(clamp(s, P.lo[0].xyz, P.hi[0].xyz), clamp(s, P.lo[1].xyz, P.hi[1].xyz), false);
    } else {
        // polynomial channels: the chroma mapping table (an MMR one: its own)
        uvec2 mm = (KIND0 != 0u || KIND1 != 0u)
                       ? mmr(clamp(s, P.lo[0].xyz, P.hi[0].xyz), clamp(s, P.lo[1].xyz, P.hi[1].xyz), false)
                       : uvec2(0u);
        m.x = KIND0 == 0u ? texelFetch(tables, T_CMAP + int(bp.x)).r : mm.x;
        m.y = KIND1 == 0u ? texelFetch(tables, T_CMAP + 1024 + int(bp.y)).r : mm.y;
    }
    o_chroma = uvec2(recon(m.x, int(texelFetch(tables, T_RES + 1024 + int(e.x)).r)),
                     recon(m.y, int(texelFetch(tables, T_RES + 2048 + int(e.y)).r)));
}
