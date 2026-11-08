// SPDX-License-Identifier: GPL-2.0-or-later
// The Pi engine (pi_engine.c): shared declarations of its two fragment
// passes. Every value is an integer code, computed as yblod's FEL route with
// the bilinear EL upscale computes it; only the chroma MMR has a float mode
// (MMR_MODE 0).
//
// Inputs are the decoder's own SAND128 P030 dmabufs, read as R32UI texel
// buffers: 128-byte columns `colh` rows tall, three 10-bit samples per word.
#extension GL_EXT_samplerless_texture_functions : require
#include "pack.h"

layout(std140, binding = 0) uniform Params {
    uvec4 size;        // w, h (luma), cw, ch (chroma = EL luma)
    uvec4 bl;          // luma word offset, chroma word offset, luma colh, chroma colh
    uvec4 el;          // the same for the EL
    uvec4 el_size;     // EL chroma w, h; -, -
    uvec4 area;        // x0, y0, x1, y1
    uvec4 black;       // y, cb, cr
    uvec4 pk;          // packet count, MMR shift Cb, Cr, rows the packets reach
    uvec4 lo[2];       // MMR input bounds per channel (x, y, z)
    uvec4 hi[2];
    uvec4 const64[2];  // the MMR constant, lo / hi words (.xy)
    ivec4 c32[6];      // [channel * 3 + order - 1]: coefficient k = 3..6 (modes 2, 3)
    vec4 cf[6];        // the coefficient / 2^(shift + 4) (modes 0, 4)
    vec4 cconst;       // the constant / 2^(shift + 4), Cb and Cr (modes 0, 4)
    vec4 inv;          // 1 / cw, 1 / ch (pass 2's texture sizes)
    vec4 cs[6];        // the coefficients / 2^32 (as c32; mode 3)
    vec4 cy;           // the constant / 2^32, Cb and Cr (mode 3)
    uvec4 narrow;      // route 3: the planner's narrow mask per channel
    uvec4 pw;          // route 3: chroma policy (1 top-left: rows k, k+1 at 4/4, 2/4)
    vec4 err;          // mode 4's error factor; the split guard's
    uvec4 packets[40]; // 640 bytes of metadata packets
    vec4 oc[8];        // the overlay's colour: the planner's gui_coeff (po_overlay.frag)
    uvec4 orect;       // P2: the output pixels the overlay may touch, x0, y0, x1, y1 (exclusive)
    uvec4 osrc;        // PO: the overlay's width, height, stride (words), rows bottom first
    vec4 oinv;         // P2: 1 / w, 1 / h (the overlay's texture coordinates)
} P;

// The per-frame tables (pi_engine.c T_*), 32-bit words.
layout(binding = 1) uniform usamplerBuffer tables;
layout(binding = 2) uniform usamplerBuffer tables64;   // the same buffer as RG32UI
const int T_MAP = 0, T_RES = 1024, T_CMAP = 4096, T_LUTF = 6144, T_LUT64 = 12288, T_SINGLE = 24576,
          T_WIDE = 27648;

// The BL luma SAND as RG32UI (word pairs: sand_even2).
layout(binding = 13) uniform usamplerBuffer bl_y2;

int edge(int p, uint n) { return clamp(p, 0, int(n) - 1); }

// n / 3 without an integer divider (V3D has none): exact for n < 2^20.
uint div3(uint n) { return uint(float(n) * 0.33333334 + 0.1); }

// The texel index of word w of a SAND128 row (128-byte columns colh rows tall,
// 32 words a column row).
int sand_index(uint base, uint colh, uint w, uint row)
{
    uint b = w * 4u;
    return int(base + ((b & 127u) >> 2) + row * 32u + (b >> 7) * 32u * colh);
}

// Samples 2q, 2q + 1 of a SAND row, from the plane's RG32UI view (word
// pairs; base even): both are in the aligned word pair 2m, 2m + 1 (samples
// 6m .. 6m+5, m = q / 3), so one 8-byte fetch, never split over two pairs or
// two columns.
uvec2 sand_even2(usamplerBuffer pairs, uint base, uint colh, uint q, uint row)
{
    uint m = div3(q), s = q - 3u * m;   // sample 2q = 6m + 2s
    uvec2 a = texelFetch(pairs, sand_index(base, colh, 2u * m, row) >> 1).rg;
    // 2q: word 0 bits 0, 20 (s 0, 1), word 1 bits 10 (s 2); 2q + 1: word 0
    // bits 10 (s 0), word 1 bits 0, 20 (s 1, 2)
    return uvec2((s == 2u ? a.y : a.x) >> (s == 0u ? 0u : s == 1u ? 20u : 10u),
                 (s != 0u ? a.y : a.x) >> (s == 0u ? 10u : s == 1u ? 0u : 20u)) & 1023u;
}

// a + b, mod 2^64 (lo, hi words)
uvec2 add64(uvec2 a, uvec2 b)
{
    uint c;
    uint lo = uaddCarry(a.x, b.x, c);
    return uvec2(lo, a.y + b.y + c);
}

// recon32 / recon12: a Q16 value plus a residual -> the 12-bit code.
uint recon(uint m, int r)
{
    int total = int(m) + r;
    return total > -8 ? uint(min((min(total, 65536) + 8) >> 4, 4095)) : 0u;
}
