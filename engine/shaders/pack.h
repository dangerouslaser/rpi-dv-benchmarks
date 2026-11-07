/* The tunnel's pixel packing, one copy for the C and the GLSL side
 * (pi_engine.c's cleared bars, p2_pack.frag's finish()): 12-bit luma yy and
 * chroma c as the BGRA bytes of the HDMI tunnel frame. Valid C and GLSL.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PI_PACK_H
#define PI_PACK_H
/* The pixels one metadata packet spans: its 1024 bits three times over, a
 * bit in each pixel's chroma LSB. */
#define PI_PACKET_PIXELS 3072u
/* yy, c: 12-bit; alpha 0xff. */
#define PI_PACK(yy, c) \
    (((((c) & 15u) << 4) | ((yy) & 15u)) | (((yy) >> 4) << 8) | (((c) >> 4) << 16) | 0xff000000u)
#endif
