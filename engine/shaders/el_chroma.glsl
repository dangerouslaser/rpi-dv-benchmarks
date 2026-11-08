// SPDX-License-Identifier: GPL-2.0-or-later
// The SAND128 reads and the EL chroma's vertical filter, shared by pass 1
// (p1_chroma.frag) and pass 0 (p0_elv.frag), which computes that filter once
// per frame for the sharper EL upscales.
layout(binding = 4) uniform usamplerBuffer bl_c;
layout(binding = 6) uniform usamplerBuffer el_c;
layout(binding = 14) uniform usamplerBuffer bl_c2;   // the BL chroma as RG32UI: word pairs
// The chroma phase method (the stream's selector).
layout(constant_id = 1) const uint METHOD = 2;
// The EL upscale (pi_engine.h enum pi_el_upscale): 0 bilinear, 1 CCM
// Annex B (the horizontal 8-tap; the vertical 64/192 is both's), 2 bicubic
// (Catmull-Rom: the vertical 4-tap, the horizontal -1 9 9 -1 / 16), 3
// Lanczos-3 (6-tap vertical and horizontal, / 1024).
layout(constant_id = 13) const uint ELUP = 0;
// The device has subgroup quad operations in fragment shaders: el_vertical_quad
// shares the EL chroma rows across each 2 x 2 fragment quad.
layout(constant_id = 15) const uint QUAD = 0;

// ---- SAND reads ------------------------------------------------------------
// Every read stays in its row: a word is read only when it holds a sample
// that exists (the last column's neighbours are taken from the left).
uint word(usamplerBuffer buf, uint base, uint colh, uint w, uint row)
{
    return texelFetch(buf, sand_index(base, colh, w, row)).r;
}
// sample j (0..5) of the word pair a, b
uint pick(uint a, uint b, uint j) { return ((j < 3u ? a : b) >> (10u * (j < 3u ? j : j - 3u))) & 1023u; }
// sample n
uint sand1(usamplerBuffer buf, uint base, uint colh, uint n, uint row)
{
    uint w = div3(n);
    return (word(buf, base, colh, w, row) >> (10u * (n - 3u * w))) & 1023u;
}
// samples n .. n+3 (two words: n + 3 is in the second); n + 3 must exist
uvec4 sand4(usamplerBuffer buf, uint base, uint colh, uint n, uint row)
{
    uint w = div3(n), sub = n - 3u * w;
    uint a = word(buf, base, colh, w, row), b = word(buf, base, colh, w + 1u, row);
    return uvec4(pick(a, b, sub), pick(a, b, sub + 1u), pick(a, b, sub + 2u), pick(a, b, sub + 3u));
}
uvec2 bl_chroma(uint cx, uint row) { return sand_even2(bl_c2, P.bl.y, P.bl.w, cx, row); }

// (Cb, Cr) of chroma column x and of x + 1 (clamped to the plane: the last
// column's pair is read with its left neighbour, in the plane)
uvec4 uvq(bool enh, uint x, uint row)
{
    uint cw = enh ? P.el_size.x : P.size.z, n = 2u * min(x, cw - 2u);
    uvec4 q = enh ? sand4(el_c, P.el.y, P.el.w, n, row) : sand4(bl_c, P.bl.y, P.bl.w, n, row);
    return x + 1u < cw ? q : q.zwzw;
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

// phase_vertical_uv at EL chroma columns ex, ex + 1, for chroma row k: the
// 64/192 vertical, or (bicubic) Catmull-Rom's at the same position, even k
// -3 29 111 -9 over rows yy-2 .. yy+1, odd -9 111 29 -3 over yy-1 .. yy+2,
// / 128, floored at 0 (the horizontal filter clamps the result).
uvec4 el_vertical(uint ex, uint k)
{
    int yy = int(k >> 1);
    uint eh = P.el_size.y;
    bool even = (k & 1u) == 0u;
    uvec4 res;
    if (ELUP == 3u) {   // Lanczos-3: even k 8 -70 277 914 -136 31 over yy-3 .. yy+2, odd mirrored over yy-2 .. yy+3
        int r = even ? yy - 3 : yy - 2;
        ivec4 a = ivec4(phase(true, ex, r, eh)), b = ivec4(phase(true, ex, r + 1, eh)),
              c = ivec4(phase(true, ex, r + 2, eh)), d = ivec4(phase(true, ex, r + 3, eh)),
              f = ivec4(phase(true, ex, r + 4, eh)), g = ivec4(phase(true, ex, r + 5, eh));
        ivec4 s = even ? 8 * a - 70 * b + 277 * c + 914 * d - 136 * f + 31 * g
                       : 31 * a - 136 * b + 914 * c + 277 * d - 70 * f + 8 * g;
        res = uvec4(max((s + 512) >> 10, ivec4(0)));
    } else if (ELUP == 2u) {
        int r = even ? yy - 2 : yy - 1;
        ivec4 a = ivec4(phase(true, ex, r, eh)), b = ivec4(phase(true, ex, r + 1, eh)),
              c = ivec4(phase(true, ex, r + 2, eh)), d = ivec4(phase(true, ex, r + 3, eh));
        ivec4 s = even ? -3 * a + 29 * b + 111 * c - 9 * d : -9 * a + 111 * b + 29 * c - 3 * d;
        res = uvec4(max((s + 64) >> 7, ivec4(0)));
    } else {
        uvec4 b = phase(true, ex, yy, eh), n = phase(true, ex, even ? yy - 1 : yy + 1, eh);
        res = (192u * b + 64u * n + 128u) >> 8;
    }
    return res;
}

// ---- el_vertical, shared by a 2 x 2 fragment quad ---------------------------
// The quad's four fragments read the same EL chroma column pair X, X + 1 and,
// their rows k being 2yy and 2yy + 1, together 3 (64/192), 5 (Catmull-Rom)
// or 7 (Lanczos-3) EL chroma rows: each lane phases one or two of them and
// takes the others from its neighbours (gl_SubgroupInvocationID's quad: four
// consecutive lanes, the horizontal pairs lane ^ 1).
ivec4 quad_row(uint x, int row) { return ivec4(phase(true, x, row, P.el_size.y)); }

// el_vertical(x, k) for the quad sharing column pair x (and k >> 1).
uvec4 el_vertical_quad(uint x, uint k)
{
    int yy = int(k >> 1);
    uint odd = k & 1u, id = gl_SubgroupInvocationID, lane = id & 3u, qb = id & ~3u;
    uvec4 res;
    if (ELUP == 3u) {   // rows yy-3 .. yy+3: lane l phases yy-3+l and yy+1+l
        ivec4 s0 = quad_row(x, yy - 3 + int(lane)), s1 = quad_row(x, yy + 1 + int(lane));
        // tap t: row yy-3 + odd + t, of lane (odd + t) & 3, its row (odd + t) >> 2
        ivec4 x3 = subgroupShuffle(s0, qb | 3u), y3 = subgroupShuffle(s1, qb);
        ivec4 a = subgroupShuffle(s0, qb | odd), b = subgroupShuffle(s0, qb | (odd + 1u)),
              c = subgroupShuffle(s0, qb | (odd + 2u)), d = odd != 0u ? y3 : x3,
              f = subgroupShuffle(s1, qb | odd), g = subgroupShuffle(s1, qb | (odd + 1u));
        ivec4 s = odd == 0u ? 8 * a - 70 * b + 277 * c + 914 * d - 136 * f + 31 * g
                            : 31 * a - 136 * b + 914 * c + 277 * d - 70 * f + 8 * g;
        res = uvec4(max((s + 512) >> 10, ivec4(0)));
    } else if (ELUP == 2u) {   // rows yy-2 .. yy+2: lane l phases yy-2+l, every lane yy+2
        ivec4 s0 = quad_row(x, yy - 2 + int(lane)), s1 = quad_row(x, yy + 2);
        ivec4 x3 = subgroupShuffle(s0, qb | 3u);
        ivec4 a = subgroupShuffle(s0, qb | odd), b = subgroupShuffle(s0, qb | (odd + 1u)),
              c = subgroupShuffle(s0, qb | (odd + 2u)), d = odd != 0u ? s1 : x3;
        ivec4 s = odd == 0u ? -3 * a + 29 * b + 111 * c - 9 * d : -9 * a + 111 * b + 29 * c - 3 * d;
        res = uvec4(max((s + 64) >> 7, ivec4(0)));
    } else {   // rows yy-1 .. yy+1: the left lanes phase their other row, the right ones yy
        bool right = (uint(gl_FragCoord.x) & 1u) != 0u;   // lane ^ 1 is the fragment beside
        ivec4 v = quad_row(x, right ? yy : odd != 0u ? yy + 1 : yy - 1), h = subgroupQuadSwapHorizontal(v);
        res = uvec4((192 * (right ? v : h) + 64 * (right ? h : v) + 128) >> 8);
    }
    return res;
}
