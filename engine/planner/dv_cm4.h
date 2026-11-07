#ifndef DV_CM4_H
#define DV_CM4_H
#include <stddef.h>
#include <stdint.h>

/* Convert one original bit-packed RPU extension to its byte-aligned tunnel
 * payload. Length carries field presence; never infer it from parsed zeros.
 * Returns 0 on success. Output and size are unchanged on failure.
 * This helper does not establish sink support or full-frame compatibility. */
int dv_cm4_extension(unsigned level,const uint8_t *raw,size_t bytes,
                     uint8_t *output,size_t capacity,size_t *output_bytes);
#endif
