/*---------------------------------------------------------------------------*\

  radc_enc_v2.c

  Streaming RADE V2 neural encoder forward pass. Mirrors the layer wiring of
  CoreEncoderStatefull.forward in radae/radae/radae_base.py (DenseNet-style
  concatenation: dense1 -> 5x (GRU -> Conv1D), then z_dense), evaluated with the
  reused Opus DNN primitives. Conv1 is dilation 1, Conv2..5 dilation 2.

  V2 vs V1: latent_dim 56 (not 80) and NO latent bottleneck - z_dense is always
  linear (the V2 model is trained with bottleneck=0 + peak=True PAPR control).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_enc_v2.h"
#include "os_support.h"
#include <string.h>

void rade_init_encoder_v2(RADEEncV2State *st) {
    memset(st, 0, sizeof(*st));
}

void rade_core_encoder_v2(RADEEncV2State *st, const RADEEncV2 *model,
                          float *z, const float *features, int arch) {
    float buf[ENC_V2_DENSE1_OUT_SIZE + ENC_V2_GRU1_OUT_SIZE + ENC_V2_CONV1_OUT_SIZE
            + ENC_V2_GRU2_OUT_SIZE + ENC_V2_CONV2_OUT_SIZE + ENC_V2_GRU3_OUT_SIZE
            + ENC_V2_CONV3_OUT_SIZE + ENC_V2_GRU4_OUT_SIZE + ENC_V2_CONV4_OUT_SIZE
            + ENC_V2_GRU5_OUT_SIZE + ENC_V2_CONV5_OUT_SIZE];
    int n = 0;

    compute_generic_dense(&model->enc_v2_dense1, &buf[n], features, ACTIVATION_TANH, arch);
    n += ENC_V2_DENSE1_OUT_SIZE;

    compute_generic_gru(&model->enc_v2_gru1_input, &model->enc_v2_gru1_recurrent, st->gru1_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru1_state, ENC_V2_GRU1_OUT_SIZE); n += ENC_V2_GRU1_OUT_SIZE;
    compute_generic_conv1d(&model->enc_v2_conv1, &buf[n], st->conv1_state, buf, n, ACTIVATION_TANH, arch);
    n += ENC_V2_CONV1_OUT_SIZE;

    compute_generic_gru(&model->enc_v2_gru2_input, &model->enc_v2_gru2_recurrent, st->gru2_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru2_state, ENC_V2_GRU2_OUT_SIZE); n += ENC_V2_GRU2_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_v2_conv2, &buf[n], st->conv2_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_V2_CONV2_OUT_SIZE;

    compute_generic_gru(&model->enc_v2_gru3_input, &model->enc_v2_gru3_recurrent, st->gru3_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru3_state, ENC_V2_GRU3_OUT_SIZE); n += ENC_V2_GRU3_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_v2_conv3, &buf[n], st->conv3_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_V2_CONV3_OUT_SIZE;

    compute_generic_gru(&model->enc_v2_gru4_input, &model->enc_v2_gru4_recurrent, st->gru4_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru4_state, ENC_V2_GRU4_OUT_SIZE); n += ENC_V2_GRU4_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_v2_conv4, &buf[n], st->conv4_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_V2_CONV4_OUT_SIZE;

    compute_generic_gru(&model->enc_v2_gru5_input, &model->enc_v2_gru5_recurrent, st->gru5_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru5_state, ENC_V2_GRU5_OUT_SIZE); n += ENC_V2_GRU5_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_v2_conv5, &buf[n], st->conv5_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_V2_CONV5_OUT_SIZE;

    /* V2: bottleneck=0, so z_dense is always linear (no tanh). */
    compute_generic_dense(&model->enc_v2_zdense, z, buf, ACTIVATION_LINEAR, arch);
}
