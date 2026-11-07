#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
#extension GL_GOOGLE_include_directive : require
// Pass 0, for the FEL's sharper EL upscales (pi_engine.h PI_EL_ANNEX_B,
// PI_EL_BICUBIC, PI_EL_LANCZOS): one fragment per EL chroma column ex and
// chroma row k (ew x ch, over pass 1's rows), writing the EL chroma's phase
// and vertical filter there (el_vertical: Annex B's 64/192, or Catmull-Rom's
// and Lanczos-3's taps). Pass 1's horizontal filter then reads finished
// columns (el_cv) instead of filtering each of them again per sample.
#include "common.glsl"
#include "el_chroma.glsl"

layout(location = 0) out uvec2 o_v;   // (Cb, Cr); above 1023 where the filter overshoots

void main()
{
    o_v = el_vertical(uint(gl_FragCoord.x), uint(gl_FragCoord.y)).xy;
}
