// vk-micro's Vulkan inputs: IN_BUF, linear P010-like storage buffers (two
// 16-bit luma samples per word; Cb|Cr<<16 per chroma word), as yblod's engine
// reads its pictures; or IN_TEX, R16UI / RG16UI sampled images (texelFetch).
layout(push_constant) uniform PC { uint work; uint mode; uint width; uint height; } pc;

#ifdef IN_BUF
layout(std430, binding = 0) readonly buffer Luma { uint luma[]; };
layout(std430, binding = 1) readonly buffer Chroma { uint chroma[]; };
vec3 fetch(uvec2 p)
{
    uint i = p.y * pc.width + p.x;
    uint lw = luma[i >> 1];
    uint y = (i & 1u) != 0u ? lw >> 16 : lw & 0xffffu;
    uint cw = chroma[(p.y >> 1) * (pc.width >> 1) + (p.x >> 1)];
    return vec3(float(y), float(cw & 0xffffu), float(cw >> 16)) / 65535.0;
}
#else
layout(binding = 2) uniform highp usampler2D luma_tex;
layout(binding = 3) uniform highp usampler2D chroma_tex;
vec3 fetch(uvec2 p)
{
    uint y = texelFetch(luma_tex, ivec2(p), 0).r;
    uvec2 c = texelFetch(chroma_tex, ivec2(p >> 1), 0).rg;
    return vec3(float(y), float(c.x), float(c.y)) / 65535.0;
}
#endif
