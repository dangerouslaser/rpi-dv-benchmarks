#ifndef DV_COLOUR_INTERNAL_H
#define DV_COLOUR_INTERNAL_H
#include "dv_metadata.h"
typedef struct {double ycc[9],lms[9],offset[3];uint32_t policy;} dv_source_colour;
typedef struct {double nonlinear[3],linear_rgb[3],linear_lms[3];} dv_source_result;
typedef struct {double inverse_ycc[9],inverse_lms[9],offset[3];} dv_target_colour;
typedef struct {double linear[3],transport[3];uint16_t code[3];} dv_target_result;
int dv_source_colour_init(const dv_source_dm *,uint32_t,dv_source_colour *);
int dv_source_colour_sample(const dv_source_colour *,const double[3],dv_source_result *);
int dv_target_colour_init(const double[9],const double[9],const double[3],dv_target_colour *);
int dv_target_colour_sample(const dv_target_colour *,uint32_t,const double[3],dv_target_result *);
#endif
