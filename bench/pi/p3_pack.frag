#version 450
#extension GL_GOOGLE_include_directive : require
// Pass 2 (the 4:2:2 filter and the pack, fused): one fragment per 4:2:0
// chroma sample (px, k), cw x ch, writing the 2 x 2 block of output pixels
// x = 2px, 2px+1 by rows 2k, 2k+1 into the linear scanout buffer itself: two
// colour attachments over the same dmabuf, its even and its odd rows, each
// RG32UI (a pixel pair, the BGRA bytes) at half width and half height.
//
// Per pixel, exactly yblod's pack.comp (PACK 0, SOURCE 2, bilinear):
//   - luma from the BL (SAND) and the EL luma (pass 1's copy, upsampled
//     bilinear: vertical 3:1 then horizontal 1:1);
//   - its 4:2:2 chroma: the tunnel's vertical filter over pass 1's chroma rows
//     k-3 .. k+4, as sited_chroma() (CB1's Annex B phases, or the 2-tap
//     blend), Cb on even pixels, Cr on odd;
//   - the metadata packets in the first rows' chroma LSBs, and the tunnel's
//     bit packing.
// The block shares the chroma rows (both output rows' phases from one set
// of fetches) and the EL rows k-1 .. k+1 (two gathers for all four pixels).
#include "common.glsl"

layout(binding = 3) uniform usamplerBuffer bl_y;
layout(binding = 7) uniform usampler2D chroma420;   // pass 1: cw x ch, (Cb, Cr)
layout(binding = 8) uniform usampler2D el_luma;     // pass 1: cw x ch, clamp to edge
layout(location = 0) out uvec2 o_even;   // row 2k:   pixels 2px, 2px+1
layout(location = 1) out uvec2 o_odd;    // row 2k+1
layout(constant_id = 1) const uint ANNEXB = 1;
layout(constant_id = 2) const uint TOP_LEFT = 0;
// Measurement only (PI_ABLATE): 1 no luma tables, 2 no EL gathers, 4 no chroma
// rows, 8 no SAND reads, 16 the pass with no work, 32 no tunnel packing, 64 no 4:2:2 filter.
layout(constant_id = 3) const uint ABLATE = 0;

uint packet_byte(uint i) { return (P.packets[i >> 4][(i >> 2) & 3u] >> (8u * (i & 3u))) & 255u; }

// BL luma samples x, x + 1 of rows y0, y0 + 1 (one word a row unless x is a
// word's last): (row y0 x, x+1, row y0+1 x, x+1). The rows' words are 32
// apart (a 128-byte column row).
uvec4 bl_block(uint x, uint y0)
{
    if ((ABLATE & 8u) != 0u)
        return uvec4(x, x + 1u, x + y0, x) & 1023u;
    uint w = uint(float(x) * 0.33333334 + 0.1), sub = x - 3u * w, b = w * 4u;
    int i = int(P.bl.x + ((b & 127u) >> 2) + y0 * 32u + (b >> 7) * 32u * P.bl.z);
    uint a0 = texelFetch(bl_y, i).r, a1 = texelFetch(bl_y, i + 32).r;
    uvec4 v;
    if (sub == 2u) {
        // the next word: the next in the column, or the next column's first
        int j = ((b + 4u) & 127u) != 0u ? i + 1 : i - 31 + int(32u * P.bl.z);
        v = uvec4(a0 >> 20, texelFetch(bl_y, j).r, a1 >> 20, texelFetch(bl_y, j + 32).r);
    } else
        v = uvec4(a0 >> (10u * sub), a0 >> (10u * sub + 10u), a1 >> (10u * sub), a1 >> (10u * sub + 10u));
    return v & 1023u;
}

uint luma(uint base, uint e)
{
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
        if (i < P.pk.x * 3072u) {
            uint bit_index = i & 1023u;
            uint bit = (packet_byte((i / 3072u) * 128u + (bit_index >> 3)) >> (7u - (bit_index & 7u))) & 1u;
            c = (c & 4094u) | (bit ^ (uint(bitCount(yy) + bitCount(c >> 1)) & 1u));
        }
    }
    return (((c & 15u) << 4) | (yy & 15u)) | ((yy >> 4) << 8) | ((c >> 4) << 16) | 0xff000000u;
}

vec2 crow(uint cx, int k, int i, int last)
{
    if ((ABLATE & 4u) != 0u)
        return vec2(uvec2(cx + uint(i), uint(k)) & 4095u);
    return vec2(texelFetch(chroma420, ivec2(cx, clamp(k + i - 3, 0, last)), 0).rg);
}

void main()
{
    uint px = uint(gl_FragCoord.x), k = uint(gl_FragCoord.y), x = 2u * px, y0 = 2u * k;
    if ((ABLATE & 16u) != 0u) {
        o_even = uvec2(px, k);
        o_odd = uvec2(k, px);
        return;
    }

    // 4:2:2 chroma of rows 2k (e) and 2k+1 (d), (Cb, Cr) of column px
    int kk = int(k), last = int(P.size.w) - 1;
    // (no local arrays: V3D's compiler puts arrays written in a loop in
    // scratch memory before it unrolls the loop, a TMU store and load each)
    vec2 e, d;
    if (ANNEXB != 0u) {
        vec2 r0 = crow(px, kk, 0, last), r1 = crow(px, kk, 1, last), r2 = crow(px, kk, 2, last),
             r3 = crow(px, kk, 3, last), r4 = crow(px, kk, 4, last), r5 = crow(px, kk, 5, last),
             r6 = crow(px, kk, 6, last);
        if (TOP_LEFT != 0u) {
            ivec2 r7 = ivec2(crow(px, kk, 7, last));
            ivec2 s = 22 * (ivec2(r0) + r7) + 94 * (ivec2(r1) + ivec2(r6)) - 524 * (ivec2(r2) + ivec2(r5)) +
                      2456 * (ivec2(r3) + ivec2(r4));
            e = r3;
            d = vec2(clamp((s + 2048) >> 12, ivec2(0), ivec2(4095)));
        } else if ((ABLATE & 64u) != 0u) {
            e = r3;
            d = r4;
        } else {
            // exact in float: codes < 4096, the taps' absolute sum 330 (< 2^24)
            vec2 se = 2.0 * r0 - 12.0 * r1 + 65.0 * r2 + 222.0 * r3 - 25.0 * r4 + 4.0 * r5;
            vec2 so = 4.0 * r1 - 25.0 * r2 + 222.0 * r3 + 65.0 * r4 - 12.0 * r5 + 2.0 * r6;
            e = clamp(floor((se + 128.0) * (1.0 / 256.0)), 0.0, 4095.0);
            d = clamp(floor((so + 128.0) * (1.0 / 256.0)), 0.0, 4095.0);
        }
    } else {
        // the 2-tap blend over rows k-1, k, k+1
        vec2 a = crow(px, kk, 2, last), b = crow(px, kk, 3, last), c = crow(px, kk, 4, last);
        if (TOP_LEFT != 0u) {
            e = b;
            d = floor((2.0 * b + 2.0 * c + 2.0) * 0.25);
        } else {
            e = floor((a + 3.0 * b + 2.0) * 0.25);
            d = floor((3.0 * b + c + 2.0) * 0.25);
        }
    }
    uvec2 ce = uvec2(e), cd = uvec2(d);

    // EL luma rows k-1, k, k+1 at columns px, px + 1: row 2k mixes 3:1 rows
    // k and k-1, row 2k+1 rows k and k+1; odd pixels average the two columns.
    // gather: x (c0, b), y (c1, b), z (c1, a), w (c0, a); rows a, b = a, a+1
    uvec4 gu, gd;
    if ((ABLATE & 2u) != 0u)
        gu = gd = uvec4(px, k, px + 1u, k) & 1023u;
    else {
        vec2 at = vec2(float(px + 1u), float(k)) * P.inv.xy;   // rows k-1, k
        gu = textureGather(el_luma, at, 0);
        gd = textureGather(el_luma, at + vec2(0.0, P.inv.y), 0);   // rows k, k+1
    }
    uvec2 ve = (3u * gu.xy + gu.wz + 2u) >> 2, vd = (3u * gd.wz + gd.xy + 2u) >> 2;
    uvec2 ee = min(uvec2(ve.x, (ve.x + ve.y + 1u) >> 1), uvec2(1023u));
    uvec2 ed = min(uvec2(vd.x, (vd.x + vd.y + 1u) >> 1), uvec2(1023u));

    // BL luma: samples x, x + 1 of both rows
    uvec4 bb = bl_block(x, y0);
    uvec2 be = bb.xy, bd = bb.zw;
    uvec2 ye = uvec2(luma(be.x, ee.x), luma(be.y, ee.y)), yd = uvec2(luma(bd.x, ed.x), luma(bd.y, ed.y));

    // x: Cb, x + 1: Cr; the letterbox's black outside the active area
    uvec4 yy = uvec4(ye, yd), c = uvec4(ce, cd);
    if (x < P.area.x || y0 < P.area.y || x + 1u >= P.area.z || y0 + 1u >= P.area.w) {
        bool ox0 = x < P.area.x || x >= P.area.z, ox1 = x + 1u < P.area.x || x + 1u >= P.area.z;
        bvec4 out_x = bvec4(ox0, ox1, ox0, ox1);
        bvec2 out_y = bvec2(y0 < P.area.y || y0 >= P.area.w, y0 + 1u < P.area.y || y0 + 1u >= P.area.w);
        bvec4 black = bvec4(out_x.x || out_y.x, out_x.y || out_y.x, out_x.z || out_y.y, out_x.w || out_y.y);
        yy = mix(yy, uvec4(P.black.x), black);
        c = mix(c, uvec4(P.black.y, P.black.z, P.black.y, P.black.z), black);
    }
    o_even = uvec2(finish(x, y0, yy.x, c.x), finish(x + 1u, y0, yy.y, c.y));
    o_odd = uvec2(finish(x, y0 + 1u, yy.z, c.z), finish(x + 1u, y0 + 1u, yy.w, c.w));
}
