// The single-layer general mapping (the planner's route 3), exactly as
// yblod's piecewise.comp (single_reshape.cl) computes it, 64-bit integers
// emulated (DV_NO_INT64): per channel, pivots and up to 8 pieces, each a
// polynomial of its own channel or an MMR of all three; 64-bit sums where
// the planner's narrow mask allows, else 128-bit. Included by pass 1.

struct Piece { uint method, order; uvec2 bias; uvec2 coefficient[21]; };
struct Curve { uint pivot_count; uint pivots[9]; Piece piece[8]; };
layout(binding = 11, std430) readonly buffer Mapping { uint base_depth, denominator; Curve curve[3]; };

bool pw_negative(uvec2 a) { return int(a.y) < 0; }
// a * b mod 2^64, b zero-extended
uvec2 pw_mul_u32(uvec2 a, uint b) { uint hi, lo; umulExtended(a.x, b, hi, lo); return uvec2(lo, hi + a.y * b); }
// min(uint64(a) >> shift, 65535), a >= 0, 0 < shift < 64
uint pw_shr_min16(uvec2 a, uint shift)
{
    if (shift >= 32u)
        return min(a.y >> (shift - 32u), 65535u);
    if ((a.y >> shift) != 0u)
        return 65535u;
    return min((a.x >> shift) | (a.y << (32u - shift)), 65535u);
}
uint pw_map16(uvec2 v, uint shift) { return pw_negative(v) ? 0u : pw_shr_min16(v, shift); }
// uint((uint64(a) * uint64(b)) >> q), 0 < q < 64
uint pw_mulshr(uint a, uint b, uint q)
{
    uint hi, lo;
    umulExtended(a, b, hi, lo);
    return q >= 32u ? hi >> (q - 32u) : (lo >> q) | (hi << (32u - q));
}

uvec4 reshape_add(uvec4 a, uvec4 b)
{
    uvec4 value;
    uint carry = 0u;
    for (uint i = 0u; i < 4u; ++i) {
        uint t = a[i] + b[i], u = t + carry;
        carry = uint(t < a[i]) | uint(u < t);
        value[i] = u;
    }
    return value;
}

uvec4 reshape_product(uvec2 coefficient, uint feature)
{
    uint lo_hi, lo_lo, hi_hi, hi_lo, carry;
    umulExtended(coefficient.x, feature, lo_hi, lo_lo);
    umulExtended(coefficient.y, feature, hi_hi, hi_lo);
    uint middle = uaddCarry(hi_lo, lo_hi, carry);
    uint top = hi_hi + carry - (pw_negative(coefficient) ? feature : 0u);
    return uvec4(lo_lo, middle, top, (top & 0x80000000u) != 0u ? 0xffffffffu : 0u);
}

// Feature k (0..6) of order o (0..2): computed from the inputs on demand (no
// local arrays: V3D's compiler puts arrays filled in loops in scratch memory).
uint pw_feature(uvec3 s, uint bits, uint q, uint o, uint k)
{
    uint f0;
    if (k < 3u)
        f0 = s[k] << bits;
    else if (k == 3u)
        f0 = s.x * s.y;
    else if (k == 4u)
        f0 = s.x * s.z;
    else if (k == 5u)
        f0 = s.y * s.z;
    else
        f0 = pw_mulshr(s.x * s.y, s.z << bits, q);
    if (o == 0u)
        return f0;
    uint f1 = k < 3u ? s[k] * s[k] : pw_mulshr(f0, f0, q);
    return o == 1u ? f1 : pw_mulshr(f0, f1, q);
}

// Channels Cb, Cr (components 6) of input (guide, Cb, Cr): 12-bit results in .y, .z.
uvec3 reshape_tuple(uvec3 input_)
{
    uint bits = base_depth, q = 2u * bits;
    uvec3 s = input_;
    uvec3 piece;
    for (uint c = 0u; c < 3u; ++c) {
        if (s[c] >= (1u << bits))
            return uvec3(0u);
        uint n = curve[c].pivot_count, pc = n - 2u;
        for (uint pp = 0u; pp + 1u < n; ++pp)
            if (s[c] < curve[c].pivots[pp + 1u]) {
                pc = pp;
                break;
            }
        piece[c] = pc;
        s[c] = clamp(s[c], curve[c].pivots[0], curve[c].pivots[n - 1u]);
    }
    uvec3 result = uvec3(0u);
    for (uint c = 1u; c < 3u; ++c) {
        uint pc = piece[c];
        uint method = curve[c].piece[pc].method, order = curve[c].piece[pc].order;
        if ((P.narrow[c] & (1u << pc)) != 0u) {
            uvec2 value = pw_mul_u32(curve[c].piece[pc].bias, 1u << q);
            if (method == 0u) {
                value = add64(value, pw_mul_u32(curve[c].piece[pc].coefficient[0], s[c] << bits));
                if (order == 2u)
                    value = add64(value, pw_mul_u32(curve[c].piece[pc].coefficient[1], s[c] * s[c]));
            } else
                for (uint o = 0u; o < order; ++o)
                    for (uint k = 0u; k < 7u; ++k)
                        value = add64(value, pw_mul_u32(curve[c].piece[pc].coefficient[o * 7u + k],
                                                           pw_feature(s, bits, q, o, k)));
            result[c] = min((pw_map16(value, denominator + q - 16u) + 8u) >> 4, 4095u);
            continue;
        }
        uvec4 value = reshape_product(curve[c].piece[pc].bias, 1u << q);
        if (method == 0u) {
            value = reshape_add(value, reshape_product(curve[c].piece[pc].coefficient[0], s[c] << bits));
            if (order == 2u)
                value = reshape_add(value, reshape_product(curve[c].piece[pc].coefficient[1], s[c] * s[c]));
        } else
            for (uint o = 0u; o < order; ++o)
                for (uint k = 0u; k < 7u; ++k)
                    value = reshape_add(value, reshape_product(curve[c].piece[pc].coefficient[o * 7u + k],
                                                               pw_feature(s, bits, q, o, k)));
        uint mapped = 0u;
        if ((value.w & 0x80000000u) == 0u)
            mapped = value.w != 0u || value.z != 0u ? 65535u : pw_shr_min16(value.xy, denominator + q - 16u);
        result[c] = min((mapped + 8u) >> 4, 4095u);
    }
    return result;
}

// The order-2 chroma build (one piece, MMR to order 2, 20-bit features).
uvec3 reshape_order2_chroma(uvec3 input_)
{
    if (any(greaterThan(input_, uvec3(1023u))))
        return uvec3(0u);
    uvec3 s;
    for (uint c = 0u; c < 3u; ++c)
        s[c] = clamp(input_[c], curve[c].pivots[0], curve[c].pivots[curve[c].pivot_count - 1u]);
    uvec3 result = uvec3(0u);
    for (uint c = 1u; c < 3u; ++c) {
        uvec2 value = pw_mul_u32(curve[c].piece[0].bias, 1048576u);
        for (uint o = 0u; o < 2u; ++o)
            for (uint k = 0u; k < 7u; ++k)
                value = add64(value, pw_mul_u32(curve[c].piece[0].coefficient[o * 7u + k], pw_feature(s, 10u, 20u, o, k)));
        result[c] = min((pw_map16(value, denominator + 4u) + 8u) >> 4, 4095u);
    }
    return result;
}
