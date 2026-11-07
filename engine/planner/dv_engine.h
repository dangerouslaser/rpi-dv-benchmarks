#ifndef DV_ENGINE_H
#define DV_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "intel_composer_config.h"
#include "dv_metadata.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DV_ENGINE_ABI 1u
typedef enum {DV_OK=0,DV_INVALID=-1,DV_UNSUPPORTED=-2,DV_NOMEM=-3,DV_SAMPLE_RANGE=-4,DV_COLOUR_DOMAIN=-5,DV_IDENTITY=-6,DV_OBSERVER=-7,DV_BACKEND=-8} dv_status;
typedef enum {DV_HOST_MEMORY=0} dv_memory;
typedef enum {DV_PLANAR420_10=1,DV_PLANAR444_12=2} dv_format;
typedef struct {uint64_t frame_id;int64_t pts;} dv_identity;
/* Native-endian uint16 LSB-aligned codes; byte strides/spans include padding.
 * No GPU handle, P010 MSB shift, implicit range conversion or ownership transfer.
 */
typedef struct {void *data;size_t stride_bytes,size_bytes;} dv_plane;
typedef struct {uint32_t memory,format,width,height;dv_plane plane[3];dv_identity identity;} dv_image;
typedef struct {
    uint32_t abi_version;
    dv_identity identity; /* must equal BOTH decoded layer identities */
    dv_intel_composer_config composer;
    dv_source_dm source;
    uint32_t active[4]; /* left,top,right,bottom; exclusive end */
    uint32_t pq_policy; /* 0 strict; 1 historical negative-zero/positive-extension */
    uint32_t chroma_phase; /* 0 linear-left; 1 cubic-left diagnostic policy */
    uint32_t el_scaler; /* 0 nearest (2x2 replication, no EL chroma phase); 1 CCM Annex B. Not metadata-selected */
    uint32_t colour_cache; /* 0 direct; 1 exact; 2 adaptive exact */
    double target_ycc[9],target_lms[9],target_offset[3];
} dv_frame_settings;
typedef enum {DV_MAPPED=0,DV_RESIDUAL=1,DV_SUM=2,DV_RECONSTRUCTED=3} dv_stage;
/* Optional synchronous diagnostics. Temporary read-only buffers expire on
 * return; callback must not reenter the context or mutate any request memory.
 * Return zero for success. Engine itself performs no I/O/logging/exit().
 */
typedef int (*dv_stage_callback)(void *,dv_stage,unsigned,const void *,size_t,unsigned);
typedef struct {dv_stage_callback stage;void *opaque;} dv_observer;
typedef struct {uint64_t source_pq_excursions;uint32_t cache_active;} dv_statistics;
typedef struct dv_engine dv_engine;
dv_status dv_engine_create(dv_engine **out);
void dv_engine_destroy(dv_engine *engine);
/* Synchronous CPU correctness backend. One call at a time per context; distinct
 * contexts may run concurrently. Inputs/settings borrowed through return.
 * Output logical pixels, identity and stats commit only on success. Padding is
 * untouched. Output/input/output-plane overlap is rejected. Descriptors,
 * settings, stats and observer must be mutually disjoint and must not alias
 * any image pixel storage. Diagnostics may
 * already have fired when a later stage fails; observers own their side effects.
 */
dv_status dv_engine_process(dv_engine *,const dv_image *bl,const dv_image *el,
    const dv_frame_settings *,dv_image *out,dv_statistics *,const dv_observer *);
const char *dv_status_string(dv_status);
#ifdef __cplusplus
}
#endif
#endif
