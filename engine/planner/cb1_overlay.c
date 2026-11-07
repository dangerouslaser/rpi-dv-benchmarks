/* SPDX-License-Identifier: LGPL-2.1-or-later
 * CB1 overlay coefficients: the Dolby Vision-only neutral-colour subset of
 * libplacebo's pl_color_repr_decode and matrix operations (e2972fdd), as used
 * by LibreELEC's dvvk-mr-compare/Kodi adapter. No libplacebo runtime needed.
 * Called only after the planner validated the complete AVDOVIMetadata.
 */
#include <libavutil/dovi_meta.h>
#include <libavutil/rational.h>
#include <math.h>
#include <stdint.h>
typedef struct { float m[3][3]; } matrix;
static void matrix_invert(matrix *mat)
{
    double m00 = mat->m[0][0], m01 = mat->m[0][1], m02 = mat->m[0][2],
           m10 = mat->m[1][0], m11 = mat->m[1][1], m12 = mat->m[1][2],
           m20 = mat->m[2][0], m21 = mat->m[2][1], m22 = mat->m[2][2];

    // calculate the adjoint
    double a00 =  (m11 * m22 - m21 * m12);
    double a01 = -(m01 * m22 - m21 * m02);
    double a02 =  (m01 * m12 - m11 * m02);
    double a10 = -(m10 * m22 - m20 * m12);
    double a11 =  (m00 * m22 - m20 * m02);
    double a12 = -(m00 * m12 - m10 * m02);
    double a20 =  (m10 * m21 - m20 * m11);
    double a21 = -(m00 * m21 - m20 * m01);
    double a22 =  (m00 * m11 - m10 * m01);

    // calculate the determinant (as inverse == 1/det * adjoint,
    // adjoint * m == identity * det, so this calculates the det)
    double det = m00 * a00 + m10 * a01 + m20 * a02;
    det = 1.0 / det;

    mat->m[0][0] = det * a00;
    mat->m[0][1] = det * a01;
    mat->m[0][2] = det * a02;
    mat->m[1][0] = det * a10;
    mat->m[1][1] = det * a11;
    mat->m[1][2] = det * a12;
    mat->m[2][0] = det * a20;
    mat->m[2][1] = det * a21;
    mat->m[2][2] = det * a22;
}

static void matrix_mul(matrix *a, const matrix *b)
{
    float a00 = a->m[0][0], a01 = a->m[0][1], a02 = a->m[0][2],
          a10 = a->m[1][0], a11 = a->m[1][1], a12 = a->m[1][2],
          a20 = a->m[2][0], a21 = a->m[2][1], a22 = a->m[2][2];

    for (int i = 0; i < 3; i++) {
        a->m[0][i] = a00 * b->m[0][i] + a01 * b->m[1][i] + a02 * b->m[2][i];
        a->m[1][i] = a10 * b->m[0][i] + a11 * b->m[1][i] + a12 * b->m[2][i];
        a->m[2][i] = a20 * b->m[0][i] + a21 * b->m[1][i] + a22 * b->m[2][i];
    }
}

int dvvk_cb1_coeff(const void *metadata, unsigned nv12, float out[32])
{
    const AVDOVIColorMetadata *c = av_dovi_get_color(metadata);
    matrix decode, source, linear = {{{3.06441879f,-2.16597676f,.10155818f},
        {-.65612108f,1.78554118f,-.12943749f},{.01736321f,-.04725154f,1.03004253f}}};
    float offset[3];
    for (int i=0;i<9;i++) {
        if (!c->ycc_to_rgb_matrix[i].den || !c->rgb_to_lms_matrix[i].den) return -1;
        decode.m[i/3][i%3] = av_q2d(c->ycc_to_rgb_matrix[i]);
        source.m[i/3][i%3] = av_q2d(c->rgb_to_lms_matrix[i]);
    }
    const double scale = nv12 ? 256.0/255.0 : 65536.0/65535.0;
    for (int i=0;i<3;i++) {
        if (!c->ycc_to_rgb_offset[i].den) return -1;
        offset[i] = av_q2d(c->ycc_to_rgb_offset[i]);
    }
    for (int i=0;i<3;i++) {
        out[9+i]=0;
        for(int j=0;j<3;j++) out[9+i] -= decode.m[i][j] * (offset[j] * scale);
    }
    matrix_mul(&linear,&source);
    matrix_invert(&decode); matrix_invert(&linear);
    for(int i=0;i<3;i++) for(int j=0;j<3;j++) {
        out[j*3+i]=decode.m[i][j]; out[12+j*3+i]=linear.m[i][j];
    }
    for(int i=0;i<21;i++) if(!isfinite(out[i])) return -1;
    return 0;
}
void dvvk_cb1_luts(float degamma[256], float pq[256])
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    for (int i = 0; i < 256; i++) {
        float v = (float)i / 255;
        degamma[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
        float l = powf((float)i / 255 * (203.0f / 10000.0f), m1);
        pq[i] = powf((c1 + c2 * l) / (1.0f + c3 * l), m2);
    }
}

