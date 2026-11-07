#ifndef DV_SOURCE_GEOMETRY_H
#define DV_SOURCE_GEOMETRY_H
#include <stddef.h>
#include <stdint.h>
enum {DV_SOURCE_CHROMA_CENTER_LEFT=0,DV_SOURCE_CHROMA_TOP_LEFT=1};
typedef struct {
    unsigned width,height,canvas_width,canvas_height,x,y;
    unsigned chroma_siting; /* 420 reconstructed-plane siting; experimental
                              full-height route uses this for source BL chroma
                              interpolation. Never decoded-EL siting. */
    unsigned destination_width,destination_height; /* Both zero means no resize. */
} dv_source_geometry;
/* Even placement retains alternating Cb/Cr sample identity.
 * Metadata and length are unchanged on failure. */
int dv_source_geometry_valid(const dv_source_geometry *);
unsigned dv_source_destination_width(const dv_source_geometry *);
unsigned dv_source_destination_height(const dv_source_geometry *);
int dv_source_place_metadata(const dv_source_geometry *,unsigned char [512],size_t *);
/* CB1 Master Reference's wire (the contract): the extension blocks in CB1's
 * order, then placed as dv_source_place_metadata, with an L5 always. */
int dv_source_place_metadata_cb1(const dv_source_geometry *,unsigned char [512],size_t *);
/* Presentation copy only: keep controls visible without changing movie masking.
 * Refresh on both edges, including redraws of a paused frame. */
int dv_source_overlay_metadata(unsigned char [512],size_t,int,int);
/* CB1's metadata packet id: advanced only when the payload differs from the
 * previous picture's (dvbridge_dv_metadata_output). Returns the id to send. */
typedef struct {unsigned char previous[512];size_t size;unsigned id;unsigned long long frames;} dv_packet_sequence;
unsigned dv_packet_sequence_next(dv_packet_sequence *,const unsigned char *,size_t);
/* Resize in source space before packing. Active rectangles use half-open
 * coordinates; round outward so scaling cannot discard active source pixels.
 * These helpers do not enable scaling in the renderer. */
int dv_source_map_active(const dv_source_geometry *,unsigned,unsigned,
                         const uint32_t [4],uint32_t [4]);
int dv_source_place_scaled_metadata(const dv_source_geometry *,unsigned,unsigned,
                                    unsigned char [512],size_t *);
#endif
