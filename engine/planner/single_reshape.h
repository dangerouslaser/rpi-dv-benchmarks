#ifndef DV_SINGLE_RESHAPE_H
#define DV_SINGLE_RESHAPE_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t method,order;
    int64_t constant,coefficient[3][7];
} dv_reshape_piece;
typedef struct {
    uint32_t pivot_count,pivots[9];
    dv_reshape_piece piece[8];
} dv_reshape_curve;
typedef struct {
    uint32_t base_depth,denominator;
    dv_reshape_curve curve[3];
} dv_single_reshape;
/* Mapping only: does not validate colour/transport metadata or choose sampling.
 * Integer coefficients remain exactly as supplied by FFmpeg. Transactional. */
int dv_single_reshape_from_ffmpeg(const void *,size_t,dv_single_reshape *);
/* Residual-disabled reference with explicit already-aligned component samples.
 * Uses the existing composer's Q(2*base_depth) feature truncation policy.
 * This is not a licensed decoder or a chroma/luma resampling policy. */
int dv_single_reshape_reference(const dv_single_reshape *,const uint16_t [3],uint16_t [3]);
/* Bits identify pieces whose every possible partial sum fits signed 64 bits. */
int dv_single_reshape_narrow_mask(const dv_single_reshape *,uint32_t [3]);
/* Polynomial luma and an explicitly matching 8/10-bit sample domain.
 * Does not validate image storage, chroma siting or profile metadata. */
int dv_single_reshape_surface_masks(const dv_single_reshape *,unsigned,uint32_t [3]);
/* Exactly 2^sample_depth entries; leaves output unchanged on rejection. */
int dv_single_reshape_luma_table_for_depth(const dv_single_reshape *,unsigned,uint16_t *,size_t);
/* Gate for the native-420 P010 kernel: luma must be polynomial and 10-bit.
 * The caller separately selects the explicit CCM center-left guide policy. */
int dv_single_reshape_p010_masks(const dv_single_reshape *,uint32_t [3]);
/* Exact polynomial luma lookup, prepared once per mapping; transactional. */
int dv_single_reshape_luma_table(const dv_single_reshape *,uint16_t [1024]);
/* Conservative dependency analysis: 1 if any chroma MMR term uses luma,
 * 0 if all chroma pieces are independent of it, -1 for invalid mappings. */
int dv_single_reshape_needs_luma_guide(const dv_single_reshape *);
/* Structural eligibility only; recomputes overflow guards for each mapping. */
int dv_single_reshape_order2_chroma(const dv_single_reshape *);
#endif
