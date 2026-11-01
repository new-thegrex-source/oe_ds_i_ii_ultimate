/*
 * QCELP (PureVoice, TIA/EIA/IS-733) speech decoder, extracted from FFmpeg's
 * libavcodec (qcelpdec.c, qcelpdata.h and the CELP/ACELP helpers it uses) so
 * the engine can play the phone's sounds without linking FFmpeg.
 *
 * Copyright (c) 2007 Reynaldo H. Verdejo Pinochet (FFmpeg QCELP decoder)
 * and the FFmpeg developers (celp_filters.c, acelp_filters.c,
 * acelp_vectors.c, lsp.c, float_dsp.c)
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. (Combined into the GPL engine as LGPL 2.1+ permits.)
 *
 * Changes from FFmpeg: no AVCodecContext/AVFrame plumbing (one packet in,
 * 160 float samples out), a small bit reader instead of get_bits.h, warnings
 * dropped.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "oe_qcelp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define QC_ASSERT(x) ((void)0)
#define QC_MAX(a, b) ((a) > (b) ? (a) : (b))
#define QC_MIN(a, b) ((a) > (b) ? (b) : (a))
#define QC_ABS(a) ((a) >= 0 ? (a) : (-(a)))
#define QC_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

static int qc_clip(int a, int amin, int amax)
{
    if (a < amin) return amin;
    if (a > amax) return amax;
    return a;
}

/* MSB-first bit reader (what libavcodec's get_bits does) */
typedef struct QCBits {
    const uint8_t *buf;
    int size_bits, index;
} QCBits;

static void qc_init_bits(QCBits *gb, const uint8_t *buf, int bytes)
{
    gb->buf = buf;
    gb->size_bits = bytes * 8;
    gb->index = 0;
}

static unsigned qc_get_bits(QCBits *gb, int n)
{
    unsigned v = 0;
    while (n--) {
        int bit = 0;
        if (gb->index < gb->size_bits)
            bit = (gb->buf[gb->index >> 3] >> (7 - (gb->index & 7))) & 1;
        gb->index++;
        v = (v << 1) | (unsigned)bit;
    }
    return v;
}

/* ---- float_dsp.c / acelp_vectors.c / acelp_filters.c / celp_filters.c / lsp.c ---- */

static float qc_scalarproduct(const float *v1, const float *v2, int len)
{
    float p = 0.0;
    int i;
    for (i = 0; i < len; i++)
        p += v1[i] * v2[i];
    return p;
}

static void qc_weighted_vector_sumf(float *out, const float *in_a, const float *in_b,
                                    float weight_coeff_a, float weight_coeff_b, int length)
{
    int i;
    for (i = 0; i < length; i++)
        out[i] = weight_coeff_a * in_a[i]
               + weight_coeff_b * in_b[i];
}

static void qc_adaptive_gain_control(float *out, const float *in, float speech_energ,
                                     int size, float alpha, float *gain_mem)
{
    int i;
    float postfilter_energ = qc_scalarproduct(in, in, size);
    float gain_scale_factor = 1.0;
    float mem = *gain_mem;

    if (postfilter_energ)
        gain_scale_factor = sqrt(speech_energ / postfilter_energ);

    gain_scale_factor *= 1.0 - alpha;

    for (i = 0; i < size; i++) {
        mem = alpha * mem + gain_scale_factor;
        out[i] = in[i] * mem;
    }

    *gain_mem = mem;
}

static void qc_scale_vector_to_given_sum_of_squares(float *out, const float *in,
                                                    float sum_of_squares, const int n)
{
    int i;
    float scalefactor = qc_scalarproduct(in, in, n);
    if (scalefactor)
        scalefactor = sqrt(sum_of_squares / scalefactor);
    for (i = 0; i < n; i++)
        out[i] = in[i] * scalefactor;
}

static void qc_tilt_compensation(float *mem, float tilt, float *samples, int size)
{
    float new_tilt_mem = samples[size - 1];
    int i;

    for (i = size - 1; i > 0; i--)
        samples[i] -= tilt * samples[i - 1];

    samples[0] -= tilt * *mem;
    *mem = new_tilt_mem;
}

static void qc_lp_synthesis_filterf(float *out, const float *filter_coeffs,
                                    const float *in, int buffer_length,
                                    int filter_length)
{
    /* FFmpeg's 4-at-a-time version (same rounding as libavcodec's output) */
    int i, n;
    float out0, out1, out2, out3;
    float old_out0, old_out1, old_out2, old_out3;
    float a, b, c;

    a = filter_coeffs[0];
    b = filter_coeffs[1];
    c = filter_coeffs[2];
    b -= filter_coeffs[0] * filter_coeffs[0];
    c -= filter_coeffs[1] * filter_coeffs[0];
    c -= filter_coeffs[0] * b;

    old_out0 = out[-4];
    old_out1 = out[-3];
    old_out2 = out[-2];
    old_out3 = out[-1];
    for (n = 0; n <= buffer_length - 4; n += 4) {
        float tmp0, tmp1, tmp2;
        float val;

        out0 = in[0];
        out1 = in[1];
        out2 = in[2];
        out3 = in[3];

        out0 -= filter_coeffs[2] * old_out1;
        out1 -= filter_coeffs[2] * old_out2;
        out2 -= filter_coeffs[2] * old_out3;

        out0 -= filter_coeffs[1] * old_out2;
        out1 -= filter_coeffs[1] * old_out3;

        out0 -= filter_coeffs[0] * old_out3;

        val = filter_coeffs[3];

        out0 -= val * old_out0;
        out1 -= val * old_out1;
        out2 -= val * old_out2;
        out3 -= val * old_out3;

        for (i = 5; i < filter_length; i += 2) {
            float t;
            old_out3 = out[-i];
            val = filter_coeffs[i-1];

            out0 -= val * old_out3;
            out1 -= val * old_out0;
            out2 -= val * old_out1;
            out3 -= val * old_out2;

            old_out2 = out[-i-1];

            val = filter_coeffs[i];

            out0 -= val * old_out2;
            out1 -= val * old_out3;
            out2 -= val * old_out0;
            out3 -= val * old_out1;

            t = old_out0; old_out0 = old_out2; old_out2 = t;
            old_out1 = old_out3;
        }

        tmp0 = out0;
        tmp1 = out1;
        tmp2 = out2;

        out3 -= a * tmp2;
        out2 -= a * tmp1;
        out1 -= a * tmp0;

        out3 -= b * tmp1;
        out2 -= b * tmp0;

        out3 -= c * tmp0;

        out[0] = out0;
        out[1] = out1;
        out[2] = out2;
        out[3] = out3;

        old_out0 = out0;
        old_out1 = out1;
        old_out2 = out2;
        old_out3 = out3;

        out += 4;
        in  += 4;
    }

    out -= n;
    in -= n;
    for (; n < buffer_length; n++) {
        out[n] = in[n];
        for (i = 1; i <= filter_length; i++)
            out[n] -= filter_coeffs[i-1] * out[n-i];
    }
}

static void qc_lp_zero_synthesis_filterf(float *out, const float *filter_coeffs,
                                         const float *in, int buffer_length,
                                         int filter_length)
{
    int i, n;
    for (n = 0; n < buffer_length; n++) {
        out[n] = in[n];
        for (i = 1; i <= filter_length; i++)
            out[n] += filter_coeffs[i-1] * in[n-i];
    }
}

#define MAX_LP_HALF_ORDER 10

static void qc_lsp2polyf(const double *lsp, double *f, int lp_half_order)
{
    int i, j;
    f[0] = 1.0;
    f[1] = -2 * lsp[0];
    lsp -= 2;
    for (i = 2; i <= lp_half_order; i++) {
        double val = -2 * lsp[2*i];
        f[i] = val * f[i-1] + 2*f[i-2];
        for (j = i-1; j > 1; j--)
            f[j] += f[j-1] * val + f[j-2];
        f[1] += val;
    }
}

static void qc_lspd2lpc(const double *lsp, float *lpc, int lp_half_order)
{
    double pa[MAX_LP_HALF_ORDER+1], qa[MAX_LP_HALF_ORDER+1];
    float *lpc2 = lpc + (lp_half_order << 1) - 1;

    qc_lsp2polyf(lsp,     pa, lp_half_order);
    qc_lsp2polyf(lsp + 1, qa, lp_half_order);

    while (lp_half_order--) {
        double paf = pa[lp_half_order+1] + pa[lp_half_order];
        double qaf = qa[lp_half_order+1] - qa[lp_half_order];

        lpc [ lp_half_order] = 0.5*(paf+qaf);
        lpc2[-lp_half_order] = 0.5*(paf-qaf);
    }
}

/* ---- qcelpdata.h ---- */



/**
 * Pre-calculated table for hammsinc function.
 * Only half of the table is needed because of symmetry.
 *
 * TIA/EIA/IS-733 2.4.5.2-2/3
 */
static const float qcelp_hammsinc_table[4] = { -0.006822,  0.041249, -0.143459,  0.588863};

typedef struct QCELPBitmap {
    uint8_t index;  /**< index into the QCELPContext structure */
    uint8_t bitpos; /**< position of the lowest bit in the value's byte */
    uint8_t bitlen; /**< number of bits to read */
} QCELPBitmap;

#define QCELP_OF(variable, bit, len) {offsetof(QCELPFrame, variable), bit, len}

/**
 * Bitmap unpacking tables for RATE_FULL
 *
 * TIA/EIA/IS-733 Table 2.4.7.1-1
 */
static const QCELPBitmap qcelp_rate_full_bitmap[] = {
                                // start on bit
    QCELP_OF(lspv  [ 2], 0, 3), // 265
    QCELP_OF(lspv  [ 1], 0, 7), // 262
    QCELP_OF(lspv  [ 0], 0, 6), // 255
    QCELP_OF(lspv  [ 4], 0, 6), // 249
    QCELP_OF(lspv  [ 3], 0, 6), // 243
    QCELP_OF(lspv  [ 2], 3, 4), // 237
    QCELP_OF(cbsign[ 0], 0, 1), // 233
    QCELP_OF(cbgain[ 0], 0, 4), // 232
    QCELP_OF(pfrac [ 0], 0, 1), // 228
    QCELP_OF(plag  [ 0], 0, 7), // 227
    QCELP_OF(pgain [ 0], 0, 3), // 220
    QCELP_OF(cindex[ 1], 0, 4), // 217
    QCELP_OF(cbsign[ 1], 0, 1), // 213
    QCELP_OF(cbgain[ 1], 0, 4), // 212
    QCELP_OF(cindex[ 0], 0, 7), // 208
    QCELP_OF(cbgain[ 3], 0, 1), // 201
    QCELP_OF(cindex[ 2], 0, 7), // 200
    QCELP_OF(cbsign[ 2], 0, 1), // 193
    QCELP_OF(cbgain[ 2], 0, 4), // 192
    QCELP_OF(cindex[ 1], 4, 3), // 188
    QCELP_OF(plag  [ 1], 0, 3), // 185
    QCELP_OF(pgain [ 1], 0, 3), // 182
    QCELP_OF(cindex[ 3], 0, 7), // 179
    QCELP_OF(cbsign[ 3], 0, 1), // 172
    QCELP_OF(cbgain[ 3], 1, 2), // 171
    QCELP_OF(cindex[ 4], 0, 6), // 169
    QCELP_OF(cbsign[ 4], 0, 1), // 163
    QCELP_OF(cbgain[ 4], 0, 4), // 162
    QCELP_OF(pfrac [ 1], 0, 1), // 158
    QCELP_OF(plag  [ 1], 3, 4), // 157
    QCELP_OF(cbgain[ 6], 0, 3), // 153
    QCELP_OF(cindex[ 5], 0, 7), // 150
    QCELP_OF(cbsign[ 5], 0, 1), // 143
    QCELP_OF(cbgain[ 5], 0, 4), // 142
    QCELP_OF(cindex[ 4], 6, 1), // 138
    QCELP_OF(cindex[ 7], 0, 3), // 137
    QCELP_OF(cbsign[ 7], 0, 1), // 134
    QCELP_OF(cbgain[ 7], 0, 3), // 133
    QCELP_OF(cindex[ 6], 0, 7), // 130
    QCELP_OF(cbsign[ 6], 0, 1), // 123
    QCELP_OF(cbgain[ 6], 3, 1), // 122
    QCELP_OF(cbgain[ 8], 0, 1), // 121
    QCELP_OF(pfrac [ 2], 0, 1), // 120
    QCELP_OF(plag  [ 2], 0, 7), // 119
    QCELP_OF(pgain [ 2], 0, 3), // 112
    QCELP_OF(cindex[ 7], 3, 4), // 109
    QCELP_OF(cbsign[ 9], 0, 1), // 105
    QCELP_OF(cbgain[ 9], 0, 4), // 104
    QCELP_OF(cindex[ 8], 0, 7), // 100
    QCELP_OF(cbsign[ 8], 0, 1), //  93
    QCELP_OF(cbgain[ 8], 1, 3), //  92
    QCELP_OF(cindex[10], 0, 4), //  89
    QCELP_OF(cbsign[10], 0, 1), //  85
    QCELP_OF(cbgain[10], 0, 4), //  84
    QCELP_OF(cindex[ 9], 0, 7), //  80
    QCELP_OF(pgain [ 3], 0, 2), //  73
    QCELP_OF(cindex[11], 0, 7), //  71
    QCELP_OF(cbsign[11], 0, 1), //  64
    QCELP_OF(cbgain[11], 0, 3), //  63
    QCELP_OF(cindex[10], 4, 3), //  60
    QCELP_OF(cindex[12], 0, 2), //  57
    QCELP_OF(cbsign[12], 0, 1), //  55
    QCELP_OF(cbgain[12], 0, 4), //  54
    QCELP_OF(pfrac [ 3], 0, 1), //  50
    QCELP_OF(plag  [ 3], 0, 7), //  49
    QCELP_OF(pgain [ 3], 2, 1), //  42
    QCELP_OF(cindex[13], 0, 6), //  41
    QCELP_OF(cbsign[13], 0, 1), //  35
    QCELP_OF(cbgain[13], 0, 4), //  34
    QCELP_OF(cindex[12], 2, 5), //  30
    QCELP_OF(cbgain[15], 0, 3), //  25
    QCELP_OF(cindex[14], 0, 7), //  22
    QCELP_OF(cbsign[14], 0, 1), //  15
    QCELP_OF(cbgain[14], 0, 4), //  14
    QCELP_OF(cindex[13], 6, 1), //  10
    QCELP_OF(reserved,   0, 2), //   9
    QCELP_OF(cindex[15], 0, 7), //   7
    QCELP_OF(cbsign[15], 0, 1)  //   0
};

/**
 * Bitmap unpacking tables for RATE_HALF
 *
 * TIA/EIA/IS-733 Table 2.4.7.2-1
 */
static const QCELPBitmap qcelp_rate_half_bitmap[] = {
                               // start on bit
    QCELP_OF(lspv  [2], 0, 3), // 123
    QCELP_OF(lspv  [1], 0, 7), // 120
    QCELP_OF(lspv  [0], 0, 6), // 113
    QCELP_OF(lspv  [4], 0, 6), // 107
    QCELP_OF(lspv  [3], 0, 6), // 101
    QCELP_OF(lspv  [2], 3, 4), //  95
    QCELP_OF(cbsign[0], 0, 1), //  91
    QCELP_OF(cbgain[0], 0, 4), //  90
    QCELP_OF(pfrac [0], 0, 1), //  86
    QCELP_OF(plag  [0], 0, 7), //  85
    QCELP_OF(pgain [0], 0, 3), //  78
    QCELP_OF(plag  [1], 0, 6), //  75
    QCELP_OF(pgain [1], 0, 3), //  69
    QCELP_OF(cindex[0], 0, 7), //  66
    QCELP_OF(pgain [2], 0, 2), //  59
    QCELP_OF(cindex[1], 0, 7), //  57
    QCELP_OF(cbsign[1], 0, 1), //  50
    QCELP_OF(cbgain[1], 0, 4), //  49
    QCELP_OF(pfrac [1], 0, 1), //  45
    QCELP_OF(plag  [1], 6, 1), //  44
    QCELP_OF(cindex[2], 0, 2), //  43
    QCELP_OF(cbsign[2], 0, 1), //  41
    QCELP_OF(cbgain[2], 0, 4), //  40
    QCELP_OF(pfrac [2], 0, 1), //  36
    QCELP_OF(plag  [2], 0, 7), //  35
    QCELP_OF(pgain [2], 2, 1), //  28
    QCELP_OF(pfrac [3], 0, 1), //  27
    QCELP_OF(plag  [3], 0, 7), //  26
    QCELP_OF(pgain [3], 0, 3), //  19
    QCELP_OF(cindex[2], 2, 5), //  16
    QCELP_OF(cindex[3], 0, 7), //  11
    QCELP_OF(cbsign[3], 0, 1), //   4
    QCELP_OF(cbgain[3], 0, 4)  //   3
};

/**
 * Bitmap unpacking tables for RATE_QUARTER
 *
 * TIA/EIA/IS-733 Table 2.4.7.3-1
 */
static const QCELPBitmap qcelp_rate_quarter_bitmap[] = {
                               // start on bit
    QCELP_OF(lspv  [2], 0, 3), // 53
    QCELP_OF(lspv  [1], 0, 7), // 50
    QCELP_OF(lspv  [0], 0, 6), // 43
    QCELP_OF(lspv  [4], 0, 6), // 37
    QCELP_OF(lspv  [3], 0, 6), // 31
    QCELP_OF(lspv  [2], 3, 4), // 25
    QCELP_OF(cbgain[3], 0, 4), // 21
    QCELP_OF(cbgain[2], 0, 4), // 17
    QCELP_OF(cbgain[1], 0, 4), // 13
    QCELP_OF(cbgain[0], 0, 4), //  9
    QCELP_OF(reserved,  0, 2), //  5
    QCELP_OF(cbgain[4], 0, 4)  //  3
};

/**
 * Bitmap unpacking tables for RATE_OCTAVE
 *
 * trick: CBSEED is written into QCELPContext.cbsign[15],
 * which is not used for RATE_OCTAVE.
 * CBSEED is only used to ensure the occurrence of random bit
 * patterns in the 16 first bits that are used as the seed.
 *
 * TIA/EIA/IS-733 Table 2.4.7.4-1
 */
static const QCELPBitmap qcelp_rate_octave_bitmap[] = {
                                // start on bit
    QCELP_OF(cbsign[15], 3, 1), // 19
    QCELP_OF(lspv   [0], 0, 1), // 18
    QCELP_OF(lspv   [1], 0, 1), // 17
    QCELP_OF(lspv   [2], 0, 1), // 16
    QCELP_OF(cbsign[15], 2, 1), // 15
    QCELP_OF(lspv   [3], 0, 1), // 14
    QCELP_OF(lspv   [4], 0, 1), // 13
    QCELP_OF(lspv   [5], 0, 1), // 12
    QCELP_OF(cbsign[15], 1, 1), // 11
    QCELP_OF(lspv   [6], 0, 1), // 10
    QCELP_OF(lspv   [7], 0, 1), //  9
    QCELP_OF(lspv   [8], 0, 1), //  8
    QCELP_OF(cbsign[15], 0, 1), //  7
    QCELP_OF(lspv   [9], 0, 1), //  6
    QCELP_OF(cbgain [0], 0, 2), //  5
    QCELP_OF(reserved,   0, 4)  //  3
};

/**
 * Bitmapping data position for each packet type in
 * the QCELPContext
 */
static const QCELPBitmap * const qcelp_unpacking_bitmaps_per_rate[5] = {
    NULL,                     ///< for SILENCE rate
    qcelp_rate_octave_bitmap,
    qcelp_rate_quarter_bitmap,
    qcelp_rate_half_bitmap,
    qcelp_rate_full_bitmap,
};

static const uint16_t qcelp_unpacking_bitmaps_lengths[5] = {
    0, ///< for SILENCE rate
    QC_ARRAY_ELEMS(qcelp_rate_octave_bitmap),
    QC_ARRAY_ELEMS(qcelp_rate_quarter_bitmap),
    QC_ARRAY_ELEMS(qcelp_rate_half_bitmap),
    QC_ARRAY_ELEMS(qcelp_rate_full_bitmap),
};

typedef uint16_t qcelp_vector[2];

/**
 * LSP vector quantization tables in x*10000 form
 *
 * TIA/EIA/IS-733 tables 2.4.3.2.6.3-1 through 2.4.3.2.6.3-5
 */

static const qcelp_vector qcelp_lspvq1[64]= {
{ 327, 118},{ 919, 111},{ 427, 440},{1327, 185},
{ 469,  50},{1272,  91},{ 892,  59},{1771, 193},
{ 222, 158},{1100, 127},{ 827,  55},{ 978, 791},
{ 665,  47},{ 700,1401},{ 670, 859},{1913,1048},
{ 471, 215},{1046, 125},{ 645, 298},{1599, 160},
{ 593,  39},{1187, 462},{ 749, 341},{1520, 511},
{ 290, 792},{ 909, 362},{ 753,  81},{1111,1058},
{ 519, 253},{ 828, 839},{ 685, 541},{1421,1258},
{ 386, 130},{ 962, 119},{ 542, 387},{1431, 185},
{ 526,  51},{1175, 260},{ 831, 167},{1728, 510},
{ 273, 437},{1172, 113},{ 771, 144},{1122, 751},
{ 619, 119},{ 492,1276},{ 658, 695},{1882, 615},
{ 415, 200},{1018,  88},{ 681, 339},{1436, 325},
{ 555, 122},{1042, 485},{ 826, 345},{1374, 743},
{ 383,1018},{1005, 358},{ 704,  86},{1301, 586},
{ 597, 241},{ 832, 621},{ 555, 573},{1504, 839}};

static const qcelp_vector qcelp_lspvq2[128]= {
{ 255, 293},{ 904, 219},{ 151,1211},{1447, 498},
{ 470, 253},{1559, 177},{1547, 994},{2394, 242},
{  91, 813},{ 857, 590},{ 934,1326},{1889, 282},
{ 813, 472},{1057,1494},{ 450,3315},{2163,1895},
{ 538, 532},{1399, 218},{ 146,1552},{1755, 626},
{ 822, 202},{1299, 663},{ 706,1732},{2656, 401},
{ 418, 745},{ 762,1038},{ 583,1748},{1746,1285},
{ 527,1169},{1314, 830},{ 556,2116},{1073,2321},
{ 297, 570},{ 981, 403},{ 468,1103},{1740, 243},
{ 725, 179},{1255, 474},{1374,1362},{1922, 912},
{ 285, 947},{ 930, 700},{ 593,1372},{1909, 576},
{ 588, 916},{1110,1116},{ 224,2719},{1633,2220},
{ 402, 520},{1061, 448},{ 402,1352},{1499, 775},
{ 664, 589},{1081, 727},{ 801,2206},{2165,1157},
{ 566, 802},{ 911,1116},{ 306,1703},{1792, 836},
{ 655, 999},{1061,1038},{ 298,2089},{1110,1753},
{ 361, 311},{ 970, 239},{ 265,1231},{1495, 573},
{ 566, 262},{1569, 293},{1341,1144},{2271, 544},
{ 214, 877},{ 847, 719},{ 794,1384},{2067, 274},
{ 703, 688},{1099,1306},{ 391,2947},{2024,1670},
{ 471, 525},{1245, 290},{ 264,1557},{1568, 807},
{ 718, 399},{1193, 685},{ 883,1594},{2729, 764},
{ 500, 754},{ 809,1108},{ 541,1648},{1523,1385},
{ 614,1196},{1209, 847},{ 345,2242},{1442,1747},
{ 199, 560},{1092, 194},{ 349,1253},{1653, 507},
{ 625, 354},{1376, 431},{1187,1465},{2164, 872},
{ 360, 974},{1008, 698},{ 704,1346},{2114, 452},
{ 720, 816},{1240,1089},{ 439,2475},{1498,2040},
{ 336, 718},{1213, 187},{ 451,1450},{1368, 885},
{ 592, 578},{1131, 531},{ 861,1855},{1764,1500},
{ 444, 970},{ 935, 903},{ 424,1687},{1633,1102},
{ 793, 897},{1060, 897},{ 185,2011},{1205,1855}};

static const qcelp_vector qcelp_lspvq3[128]= {
{ 225, 283},{1296, 355},{ 543, 343},{2073, 274},
{ 204,1099},{1562, 523},{1388, 161},{2784, 274},
{ 112, 849},{1870, 175},{1189, 160},{1490,1088},
{ 969,1115},{ 659,3322},{1158,1073},{3183,1363},
{ 517, 223},{1740, 223},{ 704, 387},{2637, 234},
{ 692,1005},{1287,1610},{ 952, 532},{2393, 646},
{ 490, 552},{1619, 657},{ 845, 670},{1784,2280},
{ 191,1775},{ 272,2868},{ 942, 952},{2628,1479},
{ 278, 579},{1565, 218},{ 814, 180},{2379, 187},
{ 276,1444},{1199,1223},{1200, 349},{3009, 307},
{ 312, 844},{1898, 306},{ 863, 470},{1685,1241},
{ 513,1727},{ 711,2233},{1085, 864},{3398, 527},
{ 414, 440},{1356, 612},{ 964, 147},{2173, 738},
{ 465,1292},{ 877,1749},{1104, 689},{2105,1311},
{ 580, 864},{1895, 752},{ 652, 609},{1485,1699},
{ 514,1400},{ 386,2131},{ 933, 798},{2473, 986},
{ 334, 360},{1375, 398},{ 621, 276},{2183, 280},
{ 311,1114},{1382, 807},{1284, 175},{2605, 636},
{ 230, 816},{1739, 408},{1074, 176},{1619,1120},
{ 784,1371},{ 448,3050},{1189, 880},{3039,1165},
{ 424, 241},{1672, 186},{ 815, 333},{2432, 324},
{ 584,1029},{1137,1546},{1015, 585},{2198, 995},
{ 574, 581},{1746, 647},{ 733, 740},{1938,1737},
{ 347,1710},{ 373,2429},{ 787,1061},{2439,1438},
{ 185, 536},{1489, 178},{ 703, 216},{2178, 487},
{ 154,1421},{1414, 994},{1103, 352},{3072, 473},
{ 408, 819},{2055, 168},{ 998, 354},{1917,1140},
{ 665,1799},{ 993,2213},{1234, 631},{3003, 762},
{ 373, 620},{1518, 425},{ 913, 300},{1966, 836},
{ 402,1185},{ 948,1385},{1121, 555},{1802,1509},
{ 474, 886},{1888, 610},{ 739, 585},{1231,2379},
{ 661,1335},{ 205,2211},{ 823, 822},{2480,1179}};

static const qcelp_vector qcelp_lspvq4[64]= {
{ 348, 311},{ 812,1145},{ 552, 461},{1826, 263},
{ 601, 675},{1730, 172},{1523, 193},{2449, 277},
{ 334, 668},{ 805,1441},{1319, 207},{1684, 910},
{ 582,1318},{1403,1098},{ 979, 832},{2700,1359},
{ 624, 228},{1292, 979},{ 800, 195},{2226, 285},
{ 730, 862},{1537, 601},{1115, 509},{2720, 354},
{ 218,1167},{1212,1538},{1074, 247},{1674,1710},
{ 322,2142},{1263, 777},{ 981, 556},{2119,1710},
{ 193, 596},{1035, 957},{ 694, 397},{1997, 253},
{ 743, 603},{1584, 321},{1346, 346},{2221, 708},
{ 451, 732},{1040,1415},{1184, 230},{1853, 919},
{ 310,1661},{1625, 706},{ 856, 843},{2902, 702},
{ 467, 348},{1108,1048},{ 859, 306},{1964, 463},
{ 560,1013},{1425, 533},{1142, 634},{2391, 879},
{ 397,1084},{1345,1700},{ 976, 248},{1887,1189},
{ 644,2087},{1262, 603},{ 877, 550},{2203,1307}};

static const qcelp_vector qcelp_lspvq5[64]= {
{ 360, 222},{ 820,1097},{ 601, 319},{1656, 198},
{ 604, 513},{1552, 141},{1391, 155},{2474, 261},
{ 269, 785},{1463, 646},{1123, 191},{2015, 223},
{ 785, 844},{1202,1011},{ 980, 807},{3014, 793},
{ 570, 180},{1135,1382},{ 778, 256},{1901, 179},
{ 807, 622},{1461, 458},{1231, 178},{2028, 821},
{ 387, 927},{1496,1004},{ 888, 392},{2246, 341},
{ 295,1462},{1156, 694},{1022, 473},{2226,1364},
{ 210, 478},{1029,1020},{ 722, 181},{1730, 251},
{ 730, 488},{1465, 293},{1303, 326},{2595, 387},
{ 458, 584},{1569, 742},{1029, 173},{1910, 495},
{ 605,1159},{1268, 719},{ 973, 646},{2872, 428},
{ 443, 334},{ 835,1465},{ 912, 138},{1716, 442},
{ 620, 778},{1316, 450},{1186, 335},{1446,1665},
{ 486,1050},{1675,1019},{ 880, 278},{2214, 202},
{ 539,1564},{1142, 533},{ 984, 391},{2130,1089}};

static const qcelp_vector * const qcelp_lspvq[5] = {
    qcelp_lspvq1,
    qcelp_lspvq2,
    qcelp_lspvq3,
    qcelp_lspvq4,
    qcelp_lspvq5
};

/**
 * The final gain scalefactor before clipping into a usable output float
 */
#define QCELP_SCALE 8192.

/**
 * Table for computing Ga (decoded linear codebook gain magnitude)
 *
 * @note The table could fit in int16_t in x*8 form, but it seems
 *       to be slower on x86
 *
 * TIA/EIA/IS-733 2.4.6.2.1-3
 */

static const float qcelp_g12ga[61] = {
    1.000/QCELP_SCALE,   1.125/QCELP_SCALE,   1.250/QCELP_SCALE,   1.375/QCELP_SCALE,
    1.625/QCELP_SCALE,   1.750/QCELP_SCALE,   2.000/QCELP_SCALE,   2.250/QCELP_SCALE,
    2.500/QCELP_SCALE,   2.875/QCELP_SCALE,   3.125/QCELP_SCALE,   3.500/QCELP_SCALE,
    4.000/QCELP_SCALE,   4.500/QCELP_SCALE,   5.000/QCELP_SCALE,   5.625/QCELP_SCALE,
    6.250/QCELP_SCALE,   7.125/QCELP_SCALE,   8.000/QCELP_SCALE,   8.875/QCELP_SCALE,
   10.000/QCELP_SCALE,  11.250/QCELP_SCALE,  12.625/QCELP_SCALE,  14.125/QCELP_SCALE,
   15.875/QCELP_SCALE,  17.750/QCELP_SCALE,  20.000/QCELP_SCALE,  22.375/QCELP_SCALE,
   25.125/QCELP_SCALE,  28.125/QCELP_SCALE,  31.625/QCELP_SCALE,  35.500/QCELP_SCALE,
   39.750/QCELP_SCALE,  44.625/QCELP_SCALE,  50.125/QCELP_SCALE,  56.250/QCELP_SCALE,
   63.125/QCELP_SCALE,  70.750/QCELP_SCALE,  79.375/QCELP_SCALE,  89.125/QCELP_SCALE,
  100.000/QCELP_SCALE, 112.250/QCELP_SCALE, 125.875/QCELP_SCALE, 141.250/QCELP_SCALE,
  158.500/QCELP_SCALE, 177.875/QCELP_SCALE, 199.500/QCELP_SCALE, 223.875/QCELP_SCALE,
  251.250/QCELP_SCALE, 281.875/QCELP_SCALE, 316.250/QCELP_SCALE, 354.875/QCELP_SCALE,
  398.125/QCELP_SCALE, 446.625/QCELP_SCALE, 501.125/QCELP_SCALE, 562.375/QCELP_SCALE,
  631.000/QCELP_SCALE, 708.000/QCELP_SCALE, 794.375/QCELP_SCALE, 891.250/QCELP_SCALE,
 1000.000/QCELP_SCALE};

/**
 * Circular codebook for rate 1 frames in x*100 form
 *
 * TIA/EIA/IS-733 2.4.6.1-2
 */
static const int16_t qcelp_rate_full_codebook[128] = {
     10,  -65,  -59,   12,  110,   34, -134,  157,
    104,  -84,  -34, -115,   23, -101,    3,   45,
   -101,  -16,  -59,   28,  -45,  134,  -67,   22,
     61,  -29,  226,  -26,  -55, -179,  157,  -51,
   -220,  -93,  -37,   60,  118,   74,  -48,  -95,
   -181,  111,   36,  -52, -215,   78, -112,   39,
    -17,  -47, -223,   19,   12,  -98, -142,  130,
     54, -127,   21,  -12,   39,  -48,   12,  128,
      6, -167,   82, -102,  -79,   55,  -44,   48,
    -20,  -53,    8,  -61,   11,  -70, -157, -168,
     20,  -56,  -74,   78,   33,  -63, -173,   -2,
    -75,  -53, -146,   77,   66,  -29,    9,  -75,
     65,  119,  -43,   76,  233,   98,  125, -156,
    -27,   78,   -9,  170,  176,  143, -148,   -7,
     27, -136,    5,   27,   18,  139,  204,    7,
   -184, -197,   52,   -3,   78, -189,    8,  -65
};
#define QCELP_RATE_FULL_CODEBOOK_RATIO .01

/**
 * Circular codebook for rate 1/2 frames in x*2 form
 *
 * TIA/EIA/IS-733 2.4.6.1-1
 */
static const int8_t qcelp_rate_half_codebook[128] = {
     0, -4,  0, -3,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0, -3, -2,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  5,
     0,  0,  0,  0,  0,  0,  4,  0,
     0,  3,  2,  0,  3,  4,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  3,  0,  0,
    -3,  3,  0,  0, -2,  0,  3,  0,
     0,  0,  0,  0,  0,  0, -5,  0,
     0,  0,  0,  3,  0,  0,  0,  3,
     0,  0,  0,  0,  0,  0,  0,  4,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  3,  6, -3, -4,  0, -3, -3,
     3, -3,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0
};
#define QCELP_RATE_HALF_CODEBOOK_RATIO 0.5

/**
 * sqrt(1.887) is the maximum of the pseudorandom
 * white sequence used to generate the scaled codebook
 * vector for bitrate 1/4.
 *
 * TIA/EIA/IS-733 2.4.8.1.2
 */
#define QCELP_SQRT1887 1.373681186

/**
 * Table for impulse response of BPF used to filter
 * the white excitation for bitrate 1/4 synthesis
 *
 * Only half the tables are needed because of symmetry.
 *
 * TIA/EIA/IS-733 2.4.8.1.2-1.1
 */
static const double qcelp_rnd_fir_coefs[11] = {
  -1.344519e-1, 1.735384e-2, -6.905826e-2, 2.434368e-2,
  -8.210701e-2, 3.041388e-2, -9.251384e-2, 3.501983e-2,
  -9.918777e-2, 3.749518e-2,  8.985137e-1
};

/**
 * This spread factor is used, for bitrate 1/8 and I_F_Q,
 * to force LSP frequencies to be at least 80 Hz apart.
 *
 * TIA/EIA/IS-733 2.4.3.3.2
 */
#define QCELP_LSP_SPREAD_FACTOR 0.02

/**
 * Predictor coefficient for the conversion of LSP codes
 * to LSP frequencies for 1/8 and I_F_Q
 *
 * TIA/EIA/IS-733 2.4.3.2.7-2
 */
#define QCELP_LSP_OCTAVE_PREDICTOR 29.0/32

/**
 * Initial coefficient to perform bandwidth expansion on LPC
 *
 * @note: 0.9883 looks like an approximation of 253/256.
 *
 * TIA/EIA/IS-733 2.4.3.3.6 6
 */
#define QCELP_BANDWIDTH_EXPANSION_COEFF 0.9883


/* ---- qcelpdec.c ---- */


/**
 * Initialize the speech codec according to the specification.
 *
 * TIA/EIA/IS-733 2.4.9
 */
void Qcelp_Init(QCELPContext *q)
{
    int i;

    memset(q, 0, sizeof(*q));
    for (i = 0; i < 10; i++)
        q->prev_lspf[i] = (i + 1) / 11.0;
}

/**
 * Decode the 10 quantized LSP frequencies from the LSPV/LSP
 * transmission codes of any bitrate and check for badly received packets.
 *
 * @param q the context
 * @param lspf line spectral pair frequencies
 *
 * @return 0 on success, -1 if the packet is badly received
 *
 * TIA/EIA/IS-733 2.4.3.2.6.2-2, 2.4.8.7.3
 */
static int decode_lspf(QCELPContext *q, float *lspf)
{
    int i;
    float tmp_lspf, smooth, erasure_coeff;
    const float *predictors;

    if (q->bitrate == RATE_OCTAVE || q->bitrate == I_F_Q) {
        predictors = q->prev_bitrate != RATE_OCTAVE &&
                     q->prev_bitrate != I_F_Q ? q->prev_lspf
                                              : q->predictor_lspf;

        if (q->bitrate == RATE_OCTAVE) {
            q->octave_count++;

            for (i = 0; i < 10; i++) {
                q->predictor_lspf[i] =
                             lspf[i] = (q->frame.lspv[i] ?  QCELP_LSP_SPREAD_FACTOR
                                                         : -QCELP_LSP_SPREAD_FACTOR) +
                                        predictors[i] * QCELP_LSP_OCTAVE_PREDICTOR   +
                                        (i + 1) * ((1 - QCELP_LSP_OCTAVE_PREDICTOR) / 11);
            }
            smooth = q->octave_count < 10 ? .875 : 0.1;
        } else {
            erasure_coeff = QCELP_LSP_OCTAVE_PREDICTOR;

            QC_ASSERT(q->bitrate == I_F_Q);

            if (q->erasure_count > 1)
                erasure_coeff *= q->erasure_count < 4 ? 0.9 : 0.7;

            for (i = 0; i < 10; i++) {
                q->predictor_lspf[i] =
                             lspf[i] = (i + 1) * (1 - erasure_coeff) / 11 +
                                       erasure_coeff * predictors[i];
            }
            smooth = 0.125;
        }

        // Check the stability of the LSP frequencies.
        lspf[0] = QC_MAX(lspf[0], QCELP_LSP_SPREAD_FACTOR);
        for (i = 1; i < 10; i++)
            lspf[i] = QC_MAX(lspf[i], lspf[i - 1] + QCELP_LSP_SPREAD_FACTOR);

        lspf[9] = QC_MIN(lspf[9], 1.0 - QCELP_LSP_SPREAD_FACTOR);
        for (i = 9; i > 0; i--)
            lspf[i - 1] = QC_MIN(lspf[i - 1], lspf[i] - QCELP_LSP_SPREAD_FACTOR);

        // Low-pass filter the LSP frequencies.
        qc_weighted_vector_sumf(lspf, lspf, q->prev_lspf, smooth, 1.0 - smooth, 10);
    } else {
        q->octave_count = 0;

        tmp_lspf = 0.0;
        for (i = 0; i < 5; i++) {
            lspf[2 * i + 0] = tmp_lspf += qcelp_lspvq[i][q->frame.lspv[i]][0] * 0.0001;
            lspf[2 * i + 1] = tmp_lspf += qcelp_lspvq[i][q->frame.lspv[i]][1] * 0.0001;
        }

        // Check for badly received packets.
        if (q->bitrate == RATE_QUARTER) {
            if (lspf[9] <= .70 || lspf[9] >= .97)
                return -1;
            for (i = 3; i < 10; i++)
                if (fabs(lspf[i] - lspf[i - 2]) < .08)
                    return -1;
        } else {
            if (lspf[9] <= .66 || lspf[9] >= .985)
                return -1;
            for (i = 4; i < 10; i++)
                if (fabs(lspf[i] - lspf[i - 4]) < .0931)
                    return -1;
        }
    }
    return 0;
}

/**
 * Convert codebook transmission codes to GAIN and INDEX.
 *
 * @param q the context
 * @param gain array holding the decoded gain
 *
 * TIA/EIA/IS-733 2.4.6.2
 */
static void decode_gain_and_index(QCELPContext *q, float *gain)
{
    int i, subframes_count, g1[16];
    float slope;

    if (q->bitrate >= RATE_QUARTER) {
        switch (q->bitrate) {
        case RATE_FULL: subframes_count = 16; break;
        case RATE_HALF: subframes_count =  4; break;
        default:        subframes_count =  5;
        }
        for (i = 0; i < subframes_count; i++) {
            g1[i] = 4 * q->frame.cbgain[i];
            if (q->bitrate == RATE_FULL && !((i + 1) & 3)) {
                g1[i] += qc_clip((g1[i - 1] + g1[i - 2] + g1[i - 3]) / 3 - 6, 0, 32);
            }

            gain[i] = qcelp_g12ga[g1[i]];

            if (q->frame.cbsign[i]) {
                gain[i] = -gain[i];
                q->frame.cindex[i] = (q->frame.cindex[i] - 89) & 127;
            }
        }

        q->prev_g1[0]         = g1[i - 2];
        q->prev_g1[1]         = g1[i - 1];
        q->last_codebook_gain = qcelp_g12ga[g1[i - 1]];

        if (q->bitrate == RATE_QUARTER) {
            // Provide smoothing of the unvoiced excitation energy.
            gain[7] =       gain[4];
            gain[6] = 0.4 * gain[3] + 0.6 * gain[4];
            gain[5] =       gain[3];
            gain[4] = 0.8 * gain[2] + 0.2 * gain[3];
            gain[3] = 0.2 * gain[1] + 0.8 * gain[2];
            gain[2] =       gain[1];
            gain[1] = 0.6 * gain[0] + 0.4 * gain[1];
        }
    } else if (q->bitrate != SILENCE) {
        if (q->bitrate == RATE_OCTAVE) {
            g1[0] = 2 * q->frame.cbgain[0] +
                    qc_clip((q->prev_g1[0] + q->prev_g1[1]) / 2 - 5, 0, 54);
            subframes_count = 8;
        } else {
            QC_ASSERT(q->bitrate == I_F_Q);

            g1[0] = q->prev_g1[1];
            switch (q->erasure_count) {
            case 1 : break;
            case 2 : g1[0] -= 1; break;
            case 3 : g1[0] -= 2; break;
            default: g1[0] -= 6;
            }
            if (g1[0] < 0)
                g1[0] = 0;
            subframes_count = 4;
        }
        // This interpolation is done to produce smoother background noise.
        slope = 0.5 * (qcelp_g12ga[g1[0]] - q->last_codebook_gain) / subframes_count;
        for (i = 1; i <= subframes_count; i++)
                gain[i - 1] = q->last_codebook_gain + slope * i;

        q->last_codebook_gain = gain[i - 2];
        q->prev_g1[0]         = q->prev_g1[1];
        q->prev_g1[1]         = g1[0];
    }
}

/**
 * If the received packet is Rate 1/4 a further sanity check is made of the
 * codebook gain.
 *
 * @param cbgain the unpacked cbgain array
 * @return -1 if the sanity check fails, 0 otherwise
 *
 * TIA/EIA/IS-733 2.4.8.7.3
 */
static int codebook_sanity_check_for_rate_quarter(const uint8_t *cbgain)
{
    int i, diff, prev_diff = 0;

    for (i = 1; i < 5; i++) {
        diff = cbgain[i] - cbgain[i-1];
        if (QC_ABS(diff) > 10)
            return -1;
        else if (QC_ABS(diff - prev_diff) > 12)
            return -1;
        prev_diff = diff;
    }
    return 0;
}

/**
 * Compute the scaled codebook vector Cdn From INDEX and GAIN
 * for all rates.
 *
 * The specification lacks some information here.
 *
 * TIA/EIA/IS-733 has an omission on the codebook index determination
 * formula for RATE_FULL and RATE_HALF frames at section 2.4.8.1.1. It says
 * you have to subtract the decoded index parameter from the given scaled
 * codebook vector index 'n' to get the desired circular codebook index, but
 * it does not mention that you have to clamp 'n' to [0-9] in order to get
 * RI-compliant results.
 *
 * The reason for this mistake seems to be the fact they forgot to mention you
 * have to do these calculations per codebook subframe and adjust given
 * equation values accordingly.
 *
 * @param q the context
 * @param gain array holding the 4 pitch subframe gain values
 * @param cdn_vector array for the generated scaled codebook vector
 */
static void compute_svector(QCELPContext *q, const float *gain,
                            float *cdn_vector)
{
    int i, j, k;
    uint16_t cbseed, cindex;
    float *rnd, tmp_gain, fir_filter_value;

    switch (q->bitrate) {
    case RATE_FULL:
        for (i = 0; i < 16; i++) {
            tmp_gain = gain[i] * QCELP_RATE_FULL_CODEBOOK_RATIO;
            cindex   = -q->frame.cindex[i];
            for (j = 0; j < 10; j++)
                *cdn_vector++ = tmp_gain *
                                qcelp_rate_full_codebook[cindex++ & 127];
        }
        break;
    case RATE_HALF:
        for (i = 0; i < 4; i++) {
            tmp_gain = gain[i] * QCELP_RATE_HALF_CODEBOOK_RATIO;
            cindex   = -q->frame.cindex[i];
            for (j = 0; j < 40; j++)
                *cdn_vector++ = tmp_gain *
                                qcelp_rate_half_codebook[cindex++ & 127];
        }
        break;
    case RATE_QUARTER:
        cbseed = (0x0003 & q->frame.lspv[4]) << 14 |
                 (0x003F & q->frame.lspv[3]) <<  8 |
                 (0x0060 & q->frame.lspv[2]) <<  1 |
                 (0x0007 & q->frame.lspv[1]) <<  3 |
                 (0x0038 & q->frame.lspv[0]) >>  3;
        rnd    = q->rnd_fir_filter_mem + 20;
        for (i = 0; i < 8; i++) {
            tmp_gain = gain[i] * (QCELP_SQRT1887 / 32768.0);
            for (k = 0; k < 20; k++) {
                cbseed = 521 * cbseed + 259;
                *rnd   = (int16_t) cbseed;

                    // FIR filter
                fir_filter_value = 0.0;
                for (j = 0; j < 10; j++)
                    fir_filter_value += qcelp_rnd_fir_coefs[j] *
                                        (rnd[-j] + rnd[-20+j]);

                fir_filter_value += qcelp_rnd_fir_coefs[10] * rnd[-10];
                *cdn_vector++     = tmp_gain * fir_filter_value;
                rnd++;
            }
        }
        memcpy(q->rnd_fir_filter_mem, q->rnd_fir_filter_mem + 160,
               20 * sizeof(float));
        break;
    case RATE_OCTAVE:
        cbseed = q->first16bits;
        for (i = 0; i < 8; i++) {
            tmp_gain = gain[i] * (QCELP_SQRT1887 / 32768.0);
            for (j = 0; j < 20; j++) {
                cbseed        = 521 * cbseed + 259;
                *cdn_vector++ = tmp_gain * (int16_t) cbseed;
            }
        }
        break;
    case I_F_Q:
        cbseed = -44; // random codebook index
        for (i = 0; i < 4; i++) {
            tmp_gain = gain[i] * QCELP_RATE_FULL_CODEBOOK_RATIO;
            for (j = 0; j < 40; j++)
                *cdn_vector++ = tmp_gain *
                                qcelp_rate_full_codebook[cbseed++ & 127];
        }
        break;
    case SILENCE:
        memset(cdn_vector, 0, 160 * sizeof(float));
        break;
    }
}

/**
 * Apply generic gain control.
 *
 * @param v_out output vector
 * @param v_in gain-controlled vector
 * @param v_ref vector to control gain of
 *
 * TIA/EIA/IS-733 2.4.8.3, 2.4.8.6
 */
static void apply_gain_ctrl(float *v_out, const float *v_ref, const float *v_in)
{
    int i;

    for (i = 0; i < 160; i += 40) {
        float res = qc_scalarproduct(v_ref + i, v_ref + i, 40);
        qc_scale_vector_to_given_sum_of_squares(v_out + i, v_in + i, res, 40);
    }
}

/**
 * Apply filter in pitch-subframe steps.
 *
 * @param memory buffer for the previous state of the filter
 *        - must be able to contain 303 elements
 *        - the 143 first elements are from the previous state
 *        - the next 160 are for output
 * @param v_in input filter vector
 * @param gain per-subframe gain array, each element is between 0.0 and 2.0
 * @param lag per-subframe lag array, each element is
 *        - between 16 and 143 if its corresponding pfrac is 0,
 *        - between 16 and 139 otherwise
 * @param pfrac per-subframe boolean array, 1 if the lag is fractional, 0
 *        otherwise
 *
 * @return filter output vector
 */
static const float *do_pitchfilter(float memory[303], const float v_in[160],
                                   const float gain[4], const uint8_t *lag,
                                   const uint8_t pfrac[4])
{
    int i, j;
    float *v_lag, *v_out;
    const float *v_len;

    v_out = memory + 143; // Output vector starts at memory[143].

    for (i = 0; i < 4; i++) {
        if (gain[i]) {
            v_lag = memory + 143 + 40 * i - lag[i];
            for (v_len = v_in + 40; v_in < v_len; v_in++) {
                if (pfrac[i]) { // If it is a fractional lag...
                    for (j = 0, *v_out = 0.0; j < 4; j++)
                        *v_out += qcelp_hammsinc_table[j] *
                                  (v_lag[j - 4] + v_lag[3 - j]);
                } else
                    *v_out = *v_lag;

                *v_out = *v_in + gain[i] * *v_out;

                v_lag++;
                v_out++;
            }
        } else {
            memcpy(v_out, v_in, 40 * sizeof(float));
            v_in  += 40;
            v_out += 40;
        }
    }

    memmove(memory, memory + 160, 143 * sizeof(float));
    return memory + 143;
}

/**
 * Apply pitch synthesis filter and pitch prefilter to the scaled codebook vector.
 * TIA/EIA/IS-733 2.4.5.2, 2.4.8.7.2
 *
 * @param q the context
 * @param cdn_vector the scaled codebook vector
 */
static void apply_pitch_filters(QCELPContext *q, float *cdn_vector)
{
    int i;
    const float *v_synthesis_filtered, *v_pre_filtered;

    if (q->bitrate >= RATE_HALF || q->bitrate == SILENCE ||
        (q->bitrate == I_F_Q && (q->prev_bitrate >= RATE_HALF))) {

        if (q->bitrate >= RATE_HALF) {
            // Compute gain & lag for the whole frame.
            for (i = 0; i < 4; i++) {
                q->pitch_gain[i] = q->frame.plag[i] ? (q->frame.pgain[i] + 1) * 0.25 : 0.0;

                q->pitch_lag[i] = q->frame.plag[i] + 16;
            }
        } else {
            float max_pitch_gain;

            if (q->bitrate == I_F_Q) {
                  if (q->erasure_count < 3)
                      max_pitch_gain = 0.9 - 0.3 * (q->erasure_count - 1);
                  else
                      max_pitch_gain = 0.0;
            } else {
                QC_ASSERT(q->bitrate == SILENCE);
                max_pitch_gain = 1.0;
            }
            for (i = 0; i < 4; i++)
                q->pitch_gain[i] = QC_MIN(q->pitch_gain[i], max_pitch_gain);

            memset(q->frame.pfrac, 0, sizeof(q->frame.pfrac));
        }

        // pitch synthesis filter
        v_synthesis_filtered = do_pitchfilter(q->pitch_synthesis_filter_mem,
                                              cdn_vector, q->pitch_gain,
                                              q->pitch_lag, q->frame.pfrac);

        // pitch prefilter update
        for (i = 0; i < 4; i++)
            q->pitch_gain[i] = 0.5 * QC_MIN(q->pitch_gain[i], 1.0);

        v_pre_filtered       = do_pitchfilter(q->pitch_pre_filter_mem,
                                              v_synthesis_filtered,
                                              q->pitch_gain, q->pitch_lag,
                                              q->frame.pfrac);

        apply_gain_ctrl(cdn_vector, v_synthesis_filtered, v_pre_filtered);
    } else {
        memcpy(q->pitch_synthesis_filter_mem,
               cdn_vector + 17, 143 * sizeof(float));
        memcpy(q->pitch_pre_filter_mem, cdn_vector + 17, 143 * sizeof(float));
        memset(q->pitch_gain, 0, sizeof(q->pitch_gain));
        memset(q->pitch_lag,  0, sizeof(q->pitch_lag));
    }
}

/**
 * Reconstruct LPC coefficients from the line spectral pair frequencies
 * and perform bandwidth expansion.
 *
 * @param lspf line spectral pair frequencies
 * @param lpc linear predictive coding coefficients
 *
 * @note: bandwidth_expansion_coeff could be precalculated into a table
 *        but it seems to be slower on x86
 *
 * TIA/EIA/IS-733 2.4.3.3.5
 */
static void lspf2lpc(const float *lspf, float *lpc)
{
    double lsp[10];
    double bandwidth_expansion_coeff = QCELP_BANDWIDTH_EXPANSION_COEFF;
    int i;

    for (i = 0; i < 10; i++)
        lsp[i] = cos(M_PI * lspf[i]);

    qc_lspd2lpc(lsp, lpc, 5);

    for (i = 0; i < 10; i++) {
        lpc[i]                    *= bandwidth_expansion_coeff;
        bandwidth_expansion_coeff *= QCELP_BANDWIDTH_EXPANSION_COEFF;
    }
}

/**
 * Interpolate LSP frequencies and compute LPC coefficients
 * for a given bitrate & pitch subframe.
 *
 * TIA/EIA/IS-733 2.4.3.3.4, 2.4.8.7.2
 *
 * @param q the context
 * @param curr_lspf LSP frequencies vector of the current frame
 * @param lpc float vector for the resulting LPC
 * @param subframe_num frame number in decoded stream
 */
static void interpolate_lpc(QCELPContext *q, const float *curr_lspf,
                            float *lpc, const int subframe_num)
{
    float interpolated_lspf[10];
    float weight;

    if (q->bitrate >= RATE_QUARTER)
        weight = 0.25 * (subframe_num + 1);
    else if (q->bitrate == RATE_OCTAVE && !subframe_num)
        weight = 0.625;
    else
        weight = 1.0;

    if (weight != 1.0) {
        qc_weighted_vector_sumf(interpolated_lspf, curr_lspf, q->prev_lspf,
                                weight, 1.0 - weight, 10);
        lspf2lpc(interpolated_lspf, lpc);
    } else if (q->bitrate >= RATE_QUARTER ||
               (q->bitrate == I_F_Q && !subframe_num))
        lspf2lpc(curr_lspf, lpc);
    else if (q->bitrate == SILENCE && !subframe_num)
        lspf2lpc(q->prev_lspf, lpc);
}

static qcelp_packet_rate buf_size2bitrate(const int buf_size)
{
    switch (buf_size) {
    case 35: return RATE_FULL;
    case 17: return RATE_HALF;
    case  8: return RATE_QUARTER;
    case  4: return RATE_OCTAVE;
    case  1: return SILENCE;
    }

    return I_F_Q;
}

/**
 * Determine the bitrate from the frame size and/or the first byte of the frame.
 *
 * @param avctx the AV codec context
 * @param buf_size length of the buffer
 * @param buf the buffer
 *
 * @return the bitrate on success,
 *         I_F_Q  if the bitrate cannot be satisfactorily determined
 *
 * TIA/EIA/IS-733 2.4.8.7.1
 */
static qcelp_packet_rate determine_bitrate(QCELPContext *q,
                                           const int buf_size,
                                           const uint8_t **buf)
{
    qcelp_packet_rate bitrate;

    if ((bitrate = buf_size2bitrate(buf_size)) >= 0) {
        if (bitrate > **buf) {
            /* "Claimed bitrate and buffer size mismatch" */
            q->warned_buf_mismatch_bitrate = 1;
            bitrate = **buf;
        } else if (bitrate < **buf) {
            /* "Buffer is too small for the claimed bitrate" */
            return I_F_Q;
        }
        (*buf)++;
    } else if ((bitrate = buf_size2bitrate(buf_size + 1)) >= 0) {
        /* "Bitrate byte missing, guessing bitrate from packet size" */
    } else
        return I_F_Q;

    return bitrate;
}


static void postfilter(QCELPContext *q, float *samples, float *lpc)
{
    static const float pow_0_775[10] = {
        0.775000, 0.600625, 0.465484, 0.360750, 0.279582,
        0.216676, 0.167924, 0.130141, 0.100859, 0.078166
    }, pow_0_625[10] = {
        0.625000, 0.390625, 0.244141, 0.152588, 0.095367,
        0.059605, 0.037253, 0.023283, 0.014552, 0.009095
    };
    float lpc_s[10], lpc_p[10], pole_out[170], zero_out[160];
    int n;

    for (n = 0; n < 10; n++) {
        lpc_s[n] = lpc[n] * pow_0_625[n];
        lpc_p[n] = lpc[n] * pow_0_775[n];
    }

    qc_lp_zero_synthesis_filterf(zero_out, lpc_s,
                                      q->formant_mem + 10, 160, 10);
    memcpy(pole_out, q->postfilter_synth_mem, sizeof(float) * 10);
    qc_lp_synthesis_filterf(pole_out + 10, lpc_p, zero_out, 160, 10);
    memcpy(q->postfilter_synth_mem, pole_out + 160, sizeof(float) * 10);

    qc_tilt_compensation(&q->postfilter_tilt_mem, 0.3, pole_out + 10, 160);

    qc_adaptive_gain_control(samples, pole_out + 10,
                             qc_scalarproduct(q->formant_mem + 10,
                                                      q->formant_mem + 10,
                                                      160),
                             160, 0.9375, &q->postfilter_agc_mem);
}

int Qcelp_DecodePacket(QCELPContext *q, const uint8_t *buf, int buf_size, float *outbuffer)
{
    int   i;
    QCBits gb;
    float quantized_lspf[10], lpc[10];
    float gain[16];
    float *formant_mem;

    if ((q->bitrate = determine_bitrate(q, buf_size, &buf)) == I_F_Q) {
        goto erasure;
    }

    if (q->bitrate == RATE_OCTAVE &&
        (q->first16bits = ((uint16_t)((buf)[0] << 8 | (buf)[1]))) == 0xFFFF) {
        goto erasure;
    }

    if (q->bitrate > SILENCE) {
        const QCELPBitmap *bitmaps     = qcelp_unpacking_bitmaps_per_rate[q->bitrate];
        const QCELPBitmap *bitmaps_end = qcelp_unpacking_bitmaps_per_rate[q->bitrate] +
                                         qcelp_unpacking_bitmaps_lengths[q->bitrate];
        uint8_t *unpacked_data         = (uint8_t *)&q->frame;

        qc_init_bits(&gb, buf, buf_size);

        memset(&q->frame, 0, sizeof(QCELPFrame));

        for (; bitmaps < bitmaps_end; bitmaps++)
            unpacked_data[bitmaps->index] |= qc_get_bits(&gb, bitmaps->bitlen) << bitmaps->bitpos;

        // Check for erasures/blanks on rates 1, 1/4 and 1/8.
        if (q->frame.reserved) {
            goto erasure;
        }
        if (q->bitrate == RATE_QUARTER &&
            codebook_sanity_check_for_rate_quarter(q->frame.cbgain)) {
            goto erasure;
        }

        if (q->bitrate >= RATE_HALF) {
            for (i = 0; i < 4; i++) {
                if (q->frame.pfrac[i] && q->frame.plag[i] >= 124) {
                    goto erasure;
                }
            }
        }
    }

    decode_gain_and_index(q, gain);
    compute_svector(q, gain, outbuffer);

    if (decode_lspf(q, quantized_lspf) < 0) {
        goto erasure;
    }

    apply_pitch_filters(q, outbuffer);

    if (q->bitrate == I_F_Q) {
erasure:
        q->bitrate = I_F_Q;
        q->erasure_count++;
        decode_gain_and_index(q, gain);
        compute_svector(q, gain, outbuffer);
        decode_lspf(q, quantized_lspf);
        apply_pitch_filters(q, outbuffer);
    } else
        q->erasure_count = 0;

    formant_mem = q->formant_mem + 10;
    for (i = 0; i < 4; i++) {
        interpolate_lpc(q, quantized_lspf, lpc, i);
        qc_lp_synthesis_filterf(formant_mem, lpc,
                                     outbuffer + i * 40, 40, 10);
        formant_mem += 40;
    }

    // postfilter, as per TIA/EIA/IS-733 2.4.8.6
    postfilter(q, outbuffer, lpc);

    memcpy(q->formant_mem, q->formant_mem + 160, 10 * sizeof(float));

    memcpy(q->prev_lspf, quantized_lspf, sizeof(q->prev_lspf));
    q->prev_bitrate  = q->bitrate;

    return 160;
}

