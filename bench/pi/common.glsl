// The Pi FEL engine (pi_engine.c): shared declarations of its three fragment
// passes. Every value is an integer code, computed as yblod's FEL route with
// the bilinear EL upscale (shaders in yblod-engine/vk/shaders) computes it;
// only the chroma MMR has a float mode (MMR_MODE 0).
//
// Inputs are the decoder's own SAND128 P030 dmabufs, read as R32UI texel
// buffers: 128-byte columns `colh` rows tall, three 10-bit samples per word.
#extension GL_EXT_samplerless_texture_functions : require

layout(std140, binding = 0) uniform Params {
    uvec4 size;        // w, h (luma), cw, ch (chroma = EL luma)
    uvec4 bl;          // luma word offset, chroma word offset, luma colh, chroma colh
    uvec4 el;          // the same for the EL
    uvec4 el_size;     // EL chroma w, h; -, -
    uvec4 sel;         // chroma phase method, dual, kind Cb, kind Cr
    uvec4 sel2;        // MMR order Cb, Cr; chroma_filter, top_left
    uvec4 area;        // x0, y0, x1, y1
    uvec4 black;       // y, cb, cr
    uvec4 pk;          // packet count, MMR shift Cb, Cr, rows the packets reach
    uvec4 lo[2];       // MMR input bounds per channel (x, y, z)
    uvec4 hi[2];
    uvec4 const64[2];  // the MMR constant, lo / hi words (.xy)
    ivec4 c_lo[6];     // [channel * 3 + order - 1]: coefficient k=3..6, low 16 bits (mode 1)
    ivec4 c_hi[6];     // the high 16 bits, arithmetic (mode 1)
    ivec4 c32[6];      // the coefficient (mode 2)
    vec4 cf[6];        // the coefficient / 2^(shift + 4) (mode 0)
    vec4 cconst;       // the constant / 2^(shift + 4), Cb and Cr (mode 0)
    vec4 inv;          // 1 / cw, 1 / ch (pass 1's texture sizes)
    vec4 cs[6];        // mode 3: the coefficients / 2^32 (as c32)
    vec4 cy;           // mode 3: the constant / 2^32, Cb and Cr
    uvec4 narrow;      // route 3: the planner's narrow mask per channel
    uvec4 pw;          // route 3: chroma policy (1 top-left: rows k, k+1 at 4/4, 2/4)
    vec4 err;          // mode 4: the float MMR's relative error factor
    uvec4 packets[40]; // 640 bytes of metadata packets
} P;

// The per-frame tables (pi_engine.c T_*), 32-bit words.
layout(binding = 1) uniform usamplerBuffer tables;
layout(binding = 2) uniform usamplerBuffer tables64;   // the same buffer as RG32UI
const int T_MAP = 0, T_RES = 1024, T_CMAP = 4096, T_LUTF = 6144, T_LUT64 = 12288, T_SINGLE = 24576;

int edge(int p, uint n) { return clamp(p, 0, int(n) - 1); }

// Sample n of a SAND128 P030 row (luma: pixel n; chroma: U V U V ...).
// n / 3 without an integer divider (V3D has none): exact for n < 2^20.
uint sand(usamplerBuffer buf, uint base, uint colh, uint n, uint row)
{
    uint w = uint(float(n) * 0.33333334 + 0.1);
    uint sub = n - 3u * w, b = w * 4u;
    uint word = base + ((b & 127u) >> 2) + row * 32u + (b >> 7) * 32u * colh;
    return (texelFetch(buf, int(word)).r >> (10u * sub)) & 1023u;
}

// recon32 / recon12: a Q16 value plus a residual -> the 12-bit code.
uint recon(uint m, int r)
{
    int total = int(m) + r;
    return total > -8 ? uint(min((min(total, 65536) + 8) >> 4, 4095)) : 0u;
}
