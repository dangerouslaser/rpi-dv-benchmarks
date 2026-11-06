#version 450
#extension GL_GOOGLE_include_directive : require
// vk-micro, fragment: IN_BUF|IN_TEX, one RGBA8 colour attachment (the tile buffer)
#include "fetch.glsl"
#include "core.glsl"
layout(location = 0) out vec4 o;

void main()
{
    o = dv_pixel(fetch(uvec2(gl_FragCoord.xy)), pc.work, pc.mode);
}
