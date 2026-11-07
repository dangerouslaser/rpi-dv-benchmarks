/* The input glue a player needs around the engine (pi_engine.h): chroma
 * siting, the decoder's buffer layout and the single-track Profile 7 split.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "pi_engine.h"
#include "pi_internal.h"

#include <string.h>

int pi_chroma_top_left(const AVFrame *f, int profile)
{
    return f->chroma_location == AVCHROMA_LOC_TOPLEFT || (f->chroma_location == AVCHROMA_LOC_UNSPECIFIED && profile == 7);
}

int pi_is_sand128(uint64_t m) { return (m >> 56) == PI_MOD_VENDOR_BROADCOM && (m & 0xff) == PI_MOD_SAND128; }

int pi_p7_split(const uint8_t *in, int size, int nal_size, int layer, uint8_t *out, int room)
{
    static const uint8_t start[4] = {0, 0, 0, 1};
    if (nal_size < 1 || nal_size > 4 || size < 0 || room < 0)
        return -1;
    int n = 0;
    const uint8_t *q = in, *end = in + size;
    while (end - q > nal_size) {
        uint32_t len = 0;
        for (int i = 0; i < nal_size; i++)
            len = len << 8 | q[i];
        q += nal_size;
        if (len < 2 || len > (size_t)(end - q))
            break;
        int type = (q[0] >> 1) & 63;
        if (layer && type == 63 && len > 2) {
            if (n + 4 + (int)len - 2 > room)
                return -1;
            memcpy(out + n, start, 4);
            memcpy(out + n + 4, q + 2, len - 2);
            n += 4 + (int)len - 2;
        } else if (!layer && type != 63) {
            if (n + nal_size + (int)len > room)
                return -1;
            memcpy(out + n, q - nal_size, (size_t)nal_size + len);
            n += nal_size + (int)len;
        }
        q += len;
    }
    return n;
}
