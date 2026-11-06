// vk-micro's per-pixel work, shared by every variant (Vulkan and GLES): a
// reshape-like polynomial and a 3x3 mix, WORK times, in float (mode 0) or in
// 10-bit integers (mode 1, the engine's bit-exact style). v: Y, Cb, Cr in 0..1.
vec4 dv_pixel(vec3 v, uint work, uint mode)
{
    if (mode == 0u) {
        for (uint i = 0u; i < work; i++) {
            v = v * (v * vec3(0.31, 0.27, 0.22) + vec3(0.62, 0.71, 0.69)) + vec3(0.013, 0.021, 0.017);
            v = mat3(0.71, 0.08, 0.02, 0.12, 0.83, 0.11, 0.17, 0.09, 0.87) * v;
        }
        return vec4(clamp(v, 0.0, 1.0), 1.0);
    }
    uvec3 u = uvec3(v * 1023.0 + 0.5);
    for (uint i = 0u; i < work; i++) {
        u = ((u * uvec3(613u, 701u, 577u)) >> 9u) + ((u * u) >> 12u) + uvec3(3u);
        u = (u.x * uvec3(727u, 64u, 20u) + u.y * uvec3(123u, 850u, 113u) + u.z * uvec3(174u, 110u, 891u)) >> 10u;
        u = min(u, uvec3(1023u));
    }
    return vec4(vec3(u) / 1023.0, 1.0);
}
