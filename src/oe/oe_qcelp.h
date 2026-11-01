/*
 * QCELP (PureVoice) decoder extracted from FFmpeg, see oe_qcelp.c.
 * LGPL 2.1 or later.
 */
#ifndef OE_QCELP_H
#define OE_QCELP_H

#include <stdint.h>

typedef struct QCELPFrame {
/// @name QCELP excitation codebook parameters
/// @{
    uint8_t cbsign[16]; ///< sign of the codebook gain for each codebook subframe
    uint8_t cbgain[16]; ///< unsigned codebook gain for each codebook subframe
    uint8_t cindex[16]; ///< codebook index for each codebook subframe
/// @}

/// @name QCELP pitch prediction parameters
/// @{
    uint8_t plag[4];    ///< pitch lag for each pitch subframe
    uint8_t pfrac[4];   ///< fractional pitch lag for each pitch subframe
    uint8_t pgain[4];   ///< pitch gain for each pitch subframe
/// @}

    /**
     * line spectral pair frequencies (LSP) for RATE_OCTAVE,
     * line spectral pair frequencies grouped into five vectors
     * of dimension two (LSPV) for other rates
     */
    uint8_t lspv[10];

    /**
     * reserved bits only present in bitrate 1, 1/4 and 1/8 packets
     */
    uint8_t reserved;
} QCELPFrame;

typedef enum {
    I_F_Q = -1,    /**< insufficient frame quality */
    SILENCE,
    RATE_OCTAVE,
    RATE_QUARTER,
    RATE_HALF,
    RATE_FULL
} qcelp_packet_rate;

typedef struct QCELPContext {
    qcelp_packet_rate bitrate;
    QCELPFrame        frame;    /**< unpacked data frame */

    uint8_t  erasure_count;
    uint8_t  octave_count;      /**< count the consecutive RATE_OCTAVE frames */
    float    prev_lspf[10];
    float    predictor_lspf[10];/**< LSP predictor for RATE_OCTAVE and I_F_Q */
    float    pitch_synthesis_filter_mem[303];
    float    pitch_pre_filter_mem[303];
    float    rnd_fir_filter_mem[180];
    float    formant_mem[170];
    float    last_codebook_gain;
    int      prev_g1[2];
    int      prev_bitrate;
    float    pitch_gain[4];
    uint8_t  pitch_lag[4];
    uint16_t first16bits;
    uint8_t  warned_buf_mismatch_bitrate;

    /* postfilter */
    float    postfilter_synth_mem[10];
    float    postfilter_agc_mem;
    float    postfilter_tilt_mem;
} QCELPContext;

void Qcelp_Init(QCELPContext *q);
/* Decodes one packet (rate byte + payload) into 160 float samples at 8 kHz
 * (about -1..1). Returns 160; a bad packet is concealed like FFmpeg does. */
int Qcelp_DecodePacket(QCELPContext *q, const uint8_t *buf, int buf_size, float *out);

#endif
