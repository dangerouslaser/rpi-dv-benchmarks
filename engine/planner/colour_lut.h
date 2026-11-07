#ifndef DV_COLOUR_LUT_H
#define DV_COLOUR_LUT_H
#include <stddef.h>
#define DV_DECODE_LAST 98304
#define DV_ENCODE_FIRST_EXP (-80)
#define DV_ENCODE_LAST_EXP 64
#ifndef DV_ENCODE_STEPS
#define DV_ENCODE_STEPS 4096
#endif
#define DV_ENCODE_STRIDE (DV_ENCODE_STEPS+3)
size_t dv_decode_lut_count(void);
size_t dv_encode_lut_count(void);
int dv_colour_make_luts(float *decode_pairs,float *encode_pairs);
#endif
