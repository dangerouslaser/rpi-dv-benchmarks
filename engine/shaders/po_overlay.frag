#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
#extension GL_GOOGLE_include_directive : require
// The overlay pass (PO): one fragment per overlay pixel in the area the
// overlay may cover, its premultiplied sRGB (ARGB8888) as the source's
// signal, premultiplied: (Y, Cb, Cr) codes x alpha / 4096, alpha. P2 samples
// it (bilinear) and blends. The colour is yblod's overlay_source_colour
// (source_overlay.cl) with the planner's coefficients, clamped where yblod
// rejects a colour the source can't represent.
#include "common.glsl"

layout(binding = 15) uniform usamplerBuffer overlay_in;   // ARGB8888, R32UI words
layout(location = 0) out vec4 o_code;

float oc(int i) { return P.oc[i >> 2][i & 3]; }
vec3 omat(int m, vec3 v)
{
    return vec3(oc(m) * v.x + oc(m + 1) * v.y + oc(m + 2) * v.z, oc(m + 3) * v.x + oc(m + 4) * v.y + oc(m + 5) * v.z,
                oc(m + 6) * v.x + oc(m + 7) * v.y + oc(m + 8) * v.z);
}

void main()
{
    uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);
    uint row = P.osrc.w != 0u ? P.osrc.y - 1u - y : y;
    uint v = texelFetch(overlay_in, int(row * P.osrc.z + x)).r, a8 = v >> 24;
    vec4 r = vec4(0.0);
    if (a8 != 0u) {
        // straight sRGB (R, G, B)
        vec3 s = min(vec3(uvec3(v >> 16, v >> 8, v) & 255u) / float(a8), vec3(1.0));
        vec3 lin = mix(pow((s + 0.055) / 1.055, vec3(2.4)), s / 12.92, lessThanEqual(s, vec3(0.04045))) *
                   (oc(30) / 10000.0);
        vec3 src = clamp(omat(9, omat(18, lin)), 0.0, 1.0);   // the source's linear LMS
        vec3 pw = pow(src, vec3(2610.0 / 16384.0));
        vec3 pq = pow((3424.0 / 4096.0 + (2413.0 / 128.0) * pw) / (1.0 + (2392.0 / 128.0) * pw), vec3(2523.0 / 32.0));
        vec3 codes = clamp((omat(0, pq) + vec3(oc(27), oc(28), oc(29))) * 4096.0, 0.0, 4095.0);
        float a = float(a8) / 255.0;
        r = vec4(codes * (a / 4096.0), a);
    }
    o_code = r;
}
