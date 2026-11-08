#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
#extension GL_GOOGLE_include_directive : require
// Pass 2 (the 4:2:2 filter and the pack, fused): one fragment per 4:2:0
// chroma sample (px, k), cw x ch, writing the 2 x 2 block of output pixels
// x = 2px, 2px+1 by rows 2k, 2k+1 into the linear scanout buffer itself: two
// colour attachments over the same dmabuf, its even and its odd rows, each
// RG32UI (a pixel pair, the BGRA bytes). (A 2 x 4 block per fragment, four
// attachments, fewer fetches per pixel, measured slower: 32 bytes a pixel of
// tile buffer shrinks the tiles.)
//
// Per pixel, exactly yblod's pack.comp (PACK 0, SOURCE 2, bilinear):
//   - luma from the BL (SAND) and the EL luma (pass 1's copy, upsampled
//     bilinear: vertical 3:1 then horizontal 1:1; or CCM Annex B's: a
//     vertical 4-tap, then the horizontal 8-tap);
//   - its 4:2:2 chroma: the tunnel's vertical filter over pass 1's chroma rows
//     k-3 .. k+4, as sited_chroma() (CB1's Annex B phases, or the 2-tap
//     blend), Cb on even pixels, Cr on odd;
//   - the metadata packets in the first rows' chroma LSBs, and the tunnel's
//     bit packing.
// The block shares the chroma rows (both output rows' phases from one set
// of fetches) and the EL rows k-1 .. k+1 (two gathers for all four pixels).
// OVERLAY: then the overlay (subtitles) over it, in the source's signal.
#include "common.glsl"

layout(binding = 7) uniform usampler2D chroma420;   // pass 1: cw x ch, (Cb, Cr)
layout(binding = 8) uniform usampler2D el_luma;     // pass 1: cw x ch, clamp to edge
layout(location = 0) out uvec2 o_even;   // row 2k:   pixels 2px, 2px+1
layout(location = 1) out uvec2 o_odd;    // row 2k+1
layout(constant_id = 1) const uint ANNEXB = 1;
layout(constant_id = 2) const uint TOP_LEFT = 0;
// Measurement only (pi_options.ablate): 1 no luma tables, 2 no EL gathers, 4 no chroma
// rows, 8 no SAND reads, 16 the pass with no work, 32 no tunnel packing, 64 no 4:2:2 filter.
layout(constant_id = 3) const uint ABLATE = 0;
layout(constant_id = 4) const uint BARS = 0;
// The single-layer polynomial route: luma is the BL through a 12-bit table.
layout(constant_id = 5) const uint SINGLE = 0;
// Route 3's full-height layout (yblod's PACK 422): pass 1 packed both rows'
// chroma, 4 x 12 bits, into its two targets; no vertical filter.
layout(constant_id = 6) const uint FULL422 = 0;
// The EL upscale (pi_engine.h enum pi_el_upscale): 0 bilinear, 1 Annex B,
// 2 bicubic (Catmull-Rom: Annex B's vertical, the horizontal -1 9 9 -1 / 16),
// 3 Lanczos-3 (6-tap vertical and horizontal, / 1024).
layout(constant_id = 7) const uint ELUP = 0;
// The overlay pass's (PO) premultiplied codes composited over the block,
// where P.orect says it may touch, sampled bilinearly at the output's
// pixels: 1 the sampler's filter (any scale), 2 its exact weights at half
// the output's size, 3 at the output's size (the filter's 8-bit weights and
// coordinates would mix a neighbour into a texel centre).
layout(constant_id = 8) const uint OVERLAY = 0;
layout(binding = 16) uniform sampler2D overlay_codes;   // PO: (Y, Cb, Cr) x alpha / 4096, alpha

uint packet_byte(uint i) { return (P.packets[i >> 4][(i >> 2) & 3u] >> (8u * (i & 3u))) & 255u; }

// BL luma samples x, x + 1 of rows y0, y0 + 1, x = 2px: (row y0 x, x+1, row
// y0+1 x, x+1), one 8-byte fetch a row.
uvec4 bl_block(uint px, uint y0)
{
    if ((ABLATE & 8u) != 0u)
        return uvec4(px, px + 1u, px + y0, px) & 1023u;
    return uvec4(sand_even2(bl_y2, P.bl.x, P.bl.z, px, y0), sand_even2(bl_y2, P.bl.x, P.bl.z, px, y0 + 1u));
}

uint luma(uint base, uint e)
{
    if (SINGLE != 0u)
        return texelFetch(tables, T_SINGLE + int(base)).r;
    if ((ABLATE & 1u) != 0u)
        return base + e;
    // T_MAP holds mapping + 8: recon() is then one add, shift and clamp
    int t = int(texelFetch(tables, T_MAP + int(base)).r) + int(texelFetch(tables, T_RES + int(e)).r);
    return uint(clamp(t >> 4, 0, 4095));
}

uint finish(uint x, uint y, uint yy, uint c)
{
    if ((ABLATE & 32u) != 0u)
        return yy | (c << 16);
    if (y < P.pk.w) {
        uint i = y * P.size.x + x;
        if (i < P.pk.x * PI_PACKET_PIXELS) {
            uint bit_index = i & 1023u;
            uint bit = (packet_byte((i / PI_PACKET_PIXELS) * 128u + (bit_index >> 3)) >> (7u - (bit_index & 7u))) & 1u;
            c = (c & 4094u) | (bit ^ (uint(bitCount(yy) + bitCount(c >> 1)) & 1u));
        }
    }
    return PI_PACK(yy, c);
}

vec2 crow(uint cx, int k, int i, int last)
{
    if ((ABLATE & 4u) != 0u)
        return vec2(uvec2(cx + uint(i), uint(k)) & 4095u);
    return vec2(texelFetch(chroma420, ivec2(cx, clamp(k + i - 3, 0, last)), 0).rg);
}

// The 4:2:2 phases (rows 2k: e, 2k+1: d) of one chroma column from its rows
// r0 .. r7 = k-3 .. k+4 (r7: the top-left eight-tap only).
void phases(vec2 r0, vec2 r1, vec2 r2, vec2 r3, vec2 r4, vec2 r5, vec2 r6, vec2 r7, out uvec2 e, out uvec2 d)
{
    vec2 fe, fd;
    if (ANNEXB != 0u && TOP_LEFT != 0u) {
        ivec2 s = 22 * (ivec2(r0) + ivec2(r7)) + 94 * (ivec2(r1) + ivec2(r6)) - 524 * (ivec2(r2) + ivec2(r5)) +
                  2456 * (ivec2(r3) + ivec2(r4));
        fe = r3;
        fd = vec2(clamp((s + 2048) >> 12, ivec2(0), ivec2(4095)));
    } else if (ANNEXB != 0u && (ABLATE & 64u) != 0u) {
        fe = r3;
        fd = r4;
    } else if (ANNEXB != 0u) {
        // exact in float: codes < 4096, the taps' absolute sum 330 (< 2^24)
        vec2 se = 2.0 * r0 - 12.0 * r1 + 65.0 * r2 + 222.0 * r3 - 25.0 * r4 + 4.0 * r5;
        vec2 so = 4.0 * r1 - 25.0 * r2 + 222.0 * r3 + 65.0 * r4 - 12.0 * r5 + 2.0 * r6;
        fe = clamp(floor((se + 128.0) * (1.0 / 256.0)), 0.0, 4095.0);
        fd = clamp(floor((so + 128.0) * (1.0 / 256.0)), 0.0, 4095.0);
    } else if (TOP_LEFT != 0u) {   // the 2-tap blend over rows k-1, k, k+1
        fe = r3;
        fd = floor((2.0 * r3 + 2.0 * r4 + 2.0) * 0.25);
    } else {
        fe = floor((r2 + 3.0 * r3 + 2.0) * 0.25);
        fd = floor((3.0 * r3 + r4 + 2.0) * 0.25);
    }
    e = uvec2(fe);
    d = uvec2(fd);
}

// The EL luma of a 2 x 2 block's rows 2k (from gather gu: rows k-1, k) and
// 2k+1 (gd: rows k, k+1), columns px, px+1: (row 2k x, x+1, row 2k+1 x, x+1).
// gather: x (c0, b), y (c1, b), z (c1, a), w (c0, a); rows a, b = a, a+1
uvec4 el_block(uvec4 gu, uvec4 gd)
{
    uvec2 ve = (3u * gu.xy + gu.wz + 2u) >> 2, vd = (3u * gd.wz + gd.xy + 2u) >> 2;
    return min(uvec4(ve.x, (ve.x + ve.y + 1u) >> 1, vd.x, (vd.x + vd.y + 1u) >> 1), uvec4(1023u));
}

// Annex B's vertical 4-tap (dv_annex_b_scale2x) at EL columns c0, c0 + 1:
// (even row, odd row) of each, from EL rows k-2 .. k+2: even -3 29 111 -9
// over k-2 .. k+1, odd -9 111 29 -3 over k-1 .. k+2, / 128, floored at 0.
// Three gathers (the sampler clamps to the edge, as the reference does).
ivec4 el_vpair(int c0, int k)
{
    vec2 at = vec2(float(c0 + 1), float(k - 1)) * P.inv.xy;   // columns c0, c0 + 1; rows k-2, k-1
    ivec4 a = ivec4(textureGather(el_luma, at, 0)), b = ivec4(textureGather(el_luma, at + vec2(0.0, 2.0 * P.inv.y), 0)),
          c = ivec4(textureGather(el_luma, at + vec2(0.0, 4.0 * P.inv.y), 0));
    // per column (left .wxwxw, right .zyzyz): rows k-2, k-1, k, k+1, k+2
    ivec2 r0 = a.wz, r1 = a.xy, r2 = b.wz, r3 = b.xy, r4 = c.wz;
    ivec2 ve = max((-3 * r0 + 29 * r1 + 111 * r2 - 9 * r3 + 64) >> 7, ivec2(0));
    ivec2 vo = max((-9 * r1 + 111 * r2 + 29 * r3 - 3 * r4 + 64) >> 7, ivec2(0));
    return ivec4(ve.x, vo.x, ve.y, vo.y);
}

// Annex B's EL luma for the 2 x 2 block at EL column px, row k (the order
// el_block gives): the even column the vertical filter's, the odd the
// horizontal 8-tap (22 94 -524 2456 2456 -524 94 22 / 4096) over columns
// px-3 .. px+4.
uvec4 el_annexb(int px, int k)
{
    ivec4 q0 = el_vpair(px - 3, k), q1 = el_vpair(px - 1, k), q2 = el_vpair(px + 1, k), q3 = el_vpair(px + 3, k);
    ivec2 h = 22 * (q0.xy + q3.zw) + 94 * (q0.zw + q3.xy) - 524 * (q1.xy + q2.zw) + 2456 * (q1.zw + q2.xy);
    ivec2 odd = clamp((h + 2048) >> 12, ivec2(0), ivec2(1023));
    ivec2 even = min(q1.zw, ivec2(1023));   // column px: q1's right
    return uvec4(uint(even.x), uint(odd.x), uint(even.y), uint(odd.y));
}

// Bicubic's EL luma for the 2 x 2 block at EL column px, row k: the
// vertical 4-tap, then -1 9 9 -1 / 16 over columns px-1 .. px+2 on the odd
// column.
uvec4 el_bicubic(int px, int k)
{
    ivec4 q1 = el_vpair(px - 1, k), q2 = el_vpair(px + 1, k);
    ivec2 h = -q1.xy + 9 * (q1.zw + q2.xy) - q2.zw;
    ivec2 odd = clamp((h + 8) >> 4, ivec2(0), ivec2(1023));
    ivec2 even = min(q1.zw, ivec2(1023));
    return uvec4(uint(even.x), uint(odd.x), uint(even.y), uint(odd.y));
}

// Lanczos-3's vertical at EL columns c0, c0 + 1: (even row, odd row) of
// each, from EL rows k-3 .. k+3: even 8 -70 277 914 -136 31 over k-3 .. k+2,
// odd mirrored over k-2 .. k+3, / 1024, floored at 0. Four gathers.
ivec4 el_vpair6(int c0, int k)
{
    vec2 at = vec2(float(c0 + 1), float(k - 2)) * P.inv.xy;   // columns c0, c0 + 1; rows k-3, k-2
    vec2 dy = vec2(0.0, 2.0 * P.inv.y);
    ivec4 a = ivec4(textureGather(el_luma, at, 0)), b = ivec4(textureGather(el_luma, at + dy, 0)),
          c = ivec4(textureGather(el_luma, at + 2.0 * dy, 0)), d = ivec4(textureGather(el_luma, at + 3.0 * dy, 0));
    ivec2 r0 = a.wz, r1 = a.xy, r2 = b.wz, r3 = b.xy, r4 = c.wz, r5 = c.xy, r6 = d.wz;   // rows k-3 .. k+3
    ivec2 ve = max((8 * r0 - 70 * r1 + 277 * r2 + 914 * r3 - 136 * r4 + 31 * r5 + 512) >> 10, ivec2(0));
    ivec2 vo = max((31 * r1 - 136 * r2 + 914 * r3 + 277 * r4 - 70 * r5 + 8 * r6 + 512) >> 10, ivec2(0));
    return ivec4(ve.x, vo.x, ve.y, vo.y);
}

// Lanczos-3's EL luma for the 2 x 2 block at EL column px, row k: the
// vertical 6-tap, then 25 -139 626 626 -139 25 / 1024 over columns
// px-2 .. px+3 on the odd column.
uvec4 el_lanczos(int px, int k)
{
    ivec4 qa = el_vpair6(px - 2, k), qb = el_vpair6(px, k), qc = el_vpair6(px + 2, k);
    ivec2 h = 25 * (qa.xy + qc.zw) - 139 * (qa.zw + qc.xy) + 626 * (qb.xy + qb.zw);
    ivec2 odd = clamp((h + 512) >> 10, ivec2(0), ivec2(1023));
    ivec2 even = min(qb.xy, ivec2(1023));   // column px: qb's left
    return uvec4(uint(even.x), uint(odd.x), uint(even.y), uint(odd.y));
}

uint over(uint v, float premultiplied, float a)
{
    return a > 0.0 ? uint(min(floor(float(v) * (1.0 - a) + premultiplied * 4096.0 + 0.5), 4095.0)) : v;
}

// One output row's chroma pair (Cb, Cr) under the overlay samples l, m, r at
// x - 1, x, x + 1 (x even): yblod's 1:2:1 coverage filter.
uvec2 over_chroma(uvec2 c, vec4 l, vec4 m, vec4 r)
{
    float coverage = (l.a + 2.0 * m.a + r.a) * 0.25;
    if (coverage <= 0.0)
        return c;
    vec2 add = (l.yz + 2.0 * m.yz + r.yz) * 1024.0;
    return uvec2(min(floor(vec2(c) * (1.0 - coverage) + add + 0.5), vec2(4095.0)));
}

// yblod's overlay blend (source_tunnel.cl SOURCE_OVERLAY) for the 2 x 2
// block at (x, y): each pixel's luma under its own sample, each row's
// chroma pair under the samples at x - 1, x, x + 1.
vec4 code_at(int cx, int cy)
{
    return texelFetch(overlay_codes, clamp(ivec2(cx, cy), ivec2(0), ivec2(P.osrc.xy) - 1), 0);
}

void overlay(uint x, uint y, inout uvec4 yy, inout uvec4 c)
{
    if (x + 1u < P.orect.x || x >= P.orect.z || y + 1u < P.orect.y || y >= P.orect.w)
        return;
    vec4 l0, m0, r0, l1, m1, r1;   // rows y, y + 1 at x - 1, x, x + 1
    if (OVERLAY == 3u) {
        int ix = int(x), iy = int(y);
        l0 = code_at(ix - 1, iy), m0 = code_at(ix, iy), r0 = code_at(ix + 1, iy);
        l1 = code_at(ix - 1, iy + 1), m1 = code_at(ix, iy + 1), r1 = code_at(ix + 1, iy + 1);
    } else if (OVERLAY == 2u) {
        // texel coordinates: x - 1, x, x + 1 at px - 3/4, px - 1/4, px + 1/4; rows k -+ 1/4
        int px = int(x >> 1), k = int(y >> 1);
        vec4 a0 = code_at(px - 1, k - 1), a1 = code_at(px, k - 1), a2 = code_at(px + 1, k - 1),
             b0 = code_at(px - 1, k), b1 = code_at(px, k), b2 = code_at(px + 1, k),
             d0 = code_at(px - 1, k + 1), d1 = code_at(px, k + 1), d2 = code_at(px + 1, k + 1);
        vec4 t0 = 0.25 * a0 + 0.75 * b0, t1 = 0.25 * a1 + 0.75 * b1, t2 = 0.25 * a2 + 0.75 * b2;   // row y
        vec4 s0 = 0.75 * b0 + 0.25 * d0, s1 = 0.75 * b1 + 0.25 * d1, s2 = 0.75 * b2 + 0.25 * d2;   // row y + 1
        l0 = 0.75 * t0 + 0.25 * t1, m0 = 0.25 * t0 + 0.75 * t1, r0 = 0.75 * t1 + 0.25 * t2;
        l1 = 0.75 * s0 + 0.25 * s1, m1 = 0.25 * s0 + 0.75 * s1, r1 = 0.75 * s1 + 0.25 * s2;
    } else {
        float u = (float(x) + 0.5) * P.oinv.x, v0 = (float(y) + 0.5) * P.oinv.y, v1 = v0 + P.oinv.y;
        l0 = textureLod(overlay_codes, vec2(u - P.oinv.x, v0), 0.0);
        m0 = textureLod(overlay_codes, vec2(u, v0), 0.0);
        r0 = textureLod(overlay_codes, vec2(u + P.oinv.x, v0), 0.0);
        l1 = textureLod(overlay_codes, vec2(u - P.oinv.x, v1), 0.0);
        m1 = textureLod(overlay_codes, vec2(u, v1), 0.0);
        r1 = textureLod(overlay_codes, vec2(u + P.oinv.x, v1), 0.0);
    }
    yy = uvec4(over(yy.x, m0.x, m0.a), over(yy.y, r0.x, r0.a), over(yy.z, m1.x, m1.a), over(yy.w, r1.x, r1.a));
    c = uvec4(over_chroma(c.xy, l0, m0, r0), over_chroma(c.zw, l1, m1, r1));
}

void pack_block(uint x, uint y, uvec4 yy, uvec4 c, out uvec2 top, out uvec2 bottom)
{
    if (OVERLAY != 0u)
        overlay(x, y, yy, c);
    top = uvec2(finish(x, y, yy.x, c.x), finish(x + 1u, y, yy.y, c.y));
    bottom = uvec2(finish(x, y + 1u, yy.z, c.z), finish(x + 1u, y + 1u, yy.w, c.w));
}

// A 2 x 2 block at (x, y): its luma from BL samples b and EL e, chroma c
// (Cb, Cr, Cb, Cr), the letterbox's black outside the active area, packed.
void block(uint x, uint y, uvec4 b, uvec4 e, uvec4 c, out uvec2 top, out uvec2 bottom)
{
    uvec4 yy = uvec4(luma(b.x, e.x), luma(b.y, e.y), luma(b.z, e.z), luma(b.w, e.w));
    if (x < P.area.x || y < P.area.y || x + 1u >= P.area.z || y + 1u >= P.area.w) {
        bool ox0 = x < P.area.x || x >= P.area.z, ox1 = x + 1u < P.area.x || x + 1u >= P.area.z;
        bool oy0 = y < P.area.y || y >= P.area.w, oy1 = y + 1u < P.area.y || y + 1u >= P.area.w;
        bvec4 black = bvec4(ox0 || oy0, ox1 || oy0, ox0 || oy1, ox1 || oy1);
        yy = mix(yy, uvec4(P.black.x), black);
        c = mix(c, uvec4(P.black.y, P.black.z, P.black.y, P.black.z), black);
    }
    pack_block(x, y, yy, c, top, bottom);
}

void main()
{
    uint px = uint(gl_FragCoord.x), k = uint(gl_FragCoord.y), x = 2u * px, y0 = 2u * k;
    if ((ABLATE & 16u) != 0u) {
        o_even = uvec2(px, k);
        o_odd = uvec2(k, px);
        return;
    }

    // BARS: the pipeline drawn over the letterbox's bars (rows wholly outside
    // the active area): black and the metadata packets (and the overlay),
    // nothing else fetched; pass 1 didn't reconstruct these rows. A separate
    // pipeline, so the main one's code is untouched by it.
    if (BARS != 0u) {
        pack_block(x, y0, uvec4(P.black.x), P.black.yzyz, o_even, o_odd);
        return;
    }

    // 4:2:2 chroma of rows 2k (e) and 2k+1 (d), (Cb, Cr) of column px. (No
    // local arrays: V3D's compiler puts arrays written in a loop in scratch.)
    int kk = int(k), last = int(P.size.w) - 1;
    uvec2 e, d;
    if (FULL422 != 0u) {
        uvec2 a = texelFetch(chroma420, ivec2(px, k), 0).rg;
        uint b = texelFetch(el_luma, ivec2(px, k), 0).r;
        e = uvec2(a.x & 4095u, (a.x >> 12) | ((a.y & 255u) << 4));
        d = uvec2((a.y >> 8) | ((b & 15u) << 8), b >> 4);
    } else if (ANNEXB != 0u) {
        vec2 r0 = crow(px, kk, 0, last), r1 = crow(px, kk, 1, last), r2 = crow(px, kk, 2, last),
             r3 = crow(px, kk, 3, last), r4 = crow(px, kk, 4, last), r5 = crow(px, kk, 5, last),
             r6 = crow(px, kk, 6, last);
        vec2 r7 = TOP_LEFT != 0u ? crow(px, kk, 7, last) : vec2(0.0);
        phases(r0, r1, r2, r3, r4, r5, r6, r7, e, d);
    } else {
        vec2 z = vec2(0.0);
        phases(z, z, crow(px, kk, 2, last), crow(px, kk, 3, last), crow(px, kk, 4, last), z, z, z, e, d);
    }

    // EL luma rows k-1, k, k+1 at columns px, px + 1: two gathers
    uvec4 el;
    if (SINGLE != 0u || (ABLATE & 2u) != 0u)
        el = uvec4(px, k, px + 1u, k) & 1023u;
    else if (ELUP == 1u)
        el = el_annexb(int(px), kk);
    else if (ELUP == 2u)
        el = el_bicubic(int(px), kk);
    else if (ELUP == 3u)
        el = el_lanczos(int(px), kk);
    else {
        vec2 at = vec2(float(px + 1u), float(k)) * P.inv.xy;   // rows k-1, k
        el = el_block(textureGather(el_luma, at, 0), textureGather(el_luma, at + vec2(0.0, P.inv.y), 0));
    }
    block(x, y0, bl_block(px, y0), el, uvec4(e, d), o_even, o_odd);
}
