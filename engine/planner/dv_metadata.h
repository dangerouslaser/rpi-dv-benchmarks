#ifndef DV_METADATA_H
#define DV_METADATA_H
#include <stdint.h>
/* Decoded source metadata, not raw RPU bytes or vendor register images. */
typedef struct {
    uint32_t compressed,eotf,param[3],depth,space,chroma,full_range;
    int32_t ycc[9],lms[9];uint32_t offset[3];
} dv_source_dm;
#endif
