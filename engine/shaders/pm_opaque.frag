#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
#extension GL_GOOGLE_include_directive : require
// The opacity pass (PM): one fragment per cell of 4 x 4 of P2's output
// blocks (opaque.glsl), drawn only over cells wholly in the overlay's area:
// 1 where every overlay pixel P2's composite samples for them (PO's codes'
// texels) is opaque (alpha 255: PO's alpha 1). OVERLAY 2 (the overlay at half
// the output's size): a block's samples read texels px - 1 .. px + 1, k - 1
// .. k + 1, a cell's 6 x 6; 3 (the same size): 2px - 1 .. 2px + 1, 2k ..
// 2k + 1, a cell's 9 x 8; clamped to the edge, as P2's reads are.
#include "common.glsl"

layout(binding = 15) uniform usamplerBuffer overlay_in;   // ARGB8888, R32UI words (po_overlay.frag)
layout(location = 0) out uint o_opaque;
layout(constant_id = 0) const uint OVERLAY = 2;

uint alpha(int x, int y)
{
    uint cx = uint(clamp(x, 0, int(P.osrc.x) - 1)), cy = uint(clamp(y, 0, int(P.osrc.y) - 1));
    uint row = P.osrc.w != 0u ? P.osrc.y - 1u - cy : cy;
    return texelFetch(overlay_in, int(row * P.osrc.z + cx)).r >> 24;
}

void main()
{
    int cx = int(gl_FragCoord.x), cy = int(gl_FragCoord.y);
    uint a = 255u;
    if (OVERLAY == 2u) {
        for (int j = -1; j <= 4; j++)
            for (int i = -1; i <= 4; i++)
                a = min(a, alpha(4 * cx + i, 4 * cy + j));
    } else {
        for (int j = 0; j <= 7; j++)
            for (int i = -1; i <= 7; i++)
                a = min(a, alpha(8 * cx + i, 8 * cy + j));
    }
    o_opaque = a == 255u ? 1u : 0u;
}
