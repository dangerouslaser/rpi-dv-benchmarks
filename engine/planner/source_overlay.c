#include "source_overlay.h"
#include <math.h>
#include <string.h>

int dv_overlay_colour_init(const dv_source_dm *dm, const double rgb_to_lms[9],
                           double white_nits, dv_overlay_colour *out)
{
    if (!dm || !rgb_to_lms || !out || !isfinite(white_nits) ||
        white_nits <= 0 || white_nits > 10000 || dm->compressed ||
        dm->eotf != 65535 || dm->param[0] || dm->param[1] || dm->param[2] ||
        dm->depth != 12 || (dm->space != 0 && dm->space != 2) ||
        dm->chroma || dm->full_range != 1) return -1;
    dv_overlay_colour c = {0};
    double ycc[9], lms[9], offset[3];
    for (unsigned i = 0; i < 9; ++i) {
        if (dm->ycc[i] < -32768 || dm->ycc[i] > 32767 ||
            dm->lms[i] < -32768 || dm->lms[i] > 32767 ||
            !isfinite(rgb_to_lms[i])) return -1;
        ycc[i] = (double)dm->ycc[i] / 8192;
        lms[i] = (double)dm->lms[i] / 16384;
        c.rgb_to_lms[i] = rgb_to_lms[i];
    }
    for (unsigned i = 0; i < 3; ++i)
        offset[i] = (double)dm->offset[i] / 268435456;
    if (dv_target_colour_init(ycc, lms, offset, &c.source)) return -1;
    c.white_nits = white_nits;
    *out = c;
    return 0;
}

int dv_overlay_colour_sample(const dv_overlay_colour *c, const uint8_t rgba[4],
                             double source_code[3], double *coverage)
{
    if (!c || !rgba || !source_code || !coverage) return -1;
    if (!rgba[3]) {
        memset(source_code, 0, 3 * sizeof(*source_code));
        *coverage = 0;
        return 0;
    }
    double linear[3], common[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        if (rgba[i] > rgba[3]) return -1;
        double v = (double)rgba[i] / rgba[3];
        linear[i] = (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)) *
                    (c->white_nits / 10000);
    }
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j)
            common[i] += c->rgb_to_lms[3 * i + j] * linear[j];
    dv_target_result result;
    if (dv_target_colour_sample(&c->source, 0, common, &result)) return -1;
    double codes[3];
    for (unsigned i = 0; i < 3; ++i) {
        codes[i] = result.transport[i] * 4096;
        if (!isfinite(codes[i]) || codes[i] < 0 || codes[i] > 4095) return -1;
    }
    memcpy(source_code, codes, sizeof(codes));
    *coverage = (double)rgba[3] / 255;
    return 0;
}
