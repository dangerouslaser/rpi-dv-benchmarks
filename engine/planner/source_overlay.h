#ifndef DV_SOURCE_OVERLAY_H
#define DV_SOURCE_OVERLAY_H
#include <stdint.h>
#include "dv_colour_internal.h"

/* CPU oracle for a coverage-alpha, premultiplied sRGB8 GUI texture.
 * GUI conversion only: reconstructed movie samples and metadata are not inputs.
 * rgb_to_lms maps linear GUI RGB to the source metadata's common LMS domain.
 * white_nits is explicit; the TV still applies the movie's display metadata. */
typedef struct {
    dv_target_colour source;
    double rgb_to_lms[9];
    double white_nits;
} dv_overlay_colour;
int dv_overlay_colour_init(const dv_source_dm *, const double rgb_to_lms[9],
                           double white_nits, dv_overlay_colour *);
/* Returns normalized straight coverage and unquantized 12-bit source codes.
 * Alpha zero returns zero coverage without interpreting RGB. Nonrepresentable
 * GUI colours reject; this oracle does not silently clip source-domain colour. */
int dv_overlay_colour_sample(const dv_overlay_colour *, const uint8_t rgba[4],
                             double source_code[3], double *coverage);
#endif
