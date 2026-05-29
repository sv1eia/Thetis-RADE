/*---------------------------------------------------------------------------*\

  radc_enc.c

  Streaming neural encoder forward pass. Mirrors the layer wiring of
  CoreEncoderStatefull.forward in radae/radae/radae_base.py (DenseNet-style
  concatenation: dense1 -> 5x (GRU -> Conv1D), then z_dense), evaluated with the
  reused Opus DNN primitives. Conv1 is dilation 1, Conv2..5 dilation 2.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_enc.h"
#include "os_support.h"
#include <string.h>

void rade_init_encoder(RADEEncState *st) {
    memset(st, 0, sizeof(*st));
}

void rade_core_encoder(RADEEncState *st, const RADEEnc *model,
                       float *z, const float *features, int arch, int bottleneck) {
    float buf[ENC_DENSE1_OUT_SIZE + ENC_GRU1_OUT_SIZE + ENC_CONV1_OUT_SIZE
            + ENC_GRU2_OUT_SIZE + ENC_CONV2_OUT_SIZE + ENC_GRU3_OUT_SIZE
            + ENC_CONV3_OUT_SIZE + ENC_GRU4_OUT_SIZE + ENC_CONV4_OUT_SIZE
            + ENC_GRU5_OUT_SIZE + ENC_CONV5_OUT_SIZE];
    int n = 0;

    compute_generic_dense(&model->enc_dense1, &buf[n], features, ACTIVATION_TANH, arch);
    n += ENC_DENSE1_OUT_SIZE;

    compute_generic_gru(&model->enc_gru1_input, &model->enc_gru1_recurrent, st->gru1_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru1_state, ENC_GRU1_OUT_SIZE); n += ENC_GRU1_OUT_SIZE;
    compute_generic_conv1d(&model->enc_conv1, &buf[n], st->conv1_state, buf, n, ACTIVATION_TANH, arch);
    n += ENC_CONV1_OUT_SIZE;

    compute_generic_gru(&model->enc_gru2_input, &model->enc_gru2_recurrent, st->gru2_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru2_state, ENC_GRU2_OUT_SIZE); n += ENC_GRU2_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_conv2, &buf[n], st->conv2_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_CONV2_OUT_SIZE;

    compute_generic_gru(&model->enc_gru3_input, &model->enc_gru3_recurrent, st->gru3_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru3_state, ENC_GRU3_OUT_SIZE); n += ENC_GRU3_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_conv3, &buf[n], st->conv3_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_CONV3_OUT_SIZE;

    compute_generic_gru(&model->enc_gru4_input, &model->enc_gru4_recurrent, st->gru4_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru4_state, ENC_GRU4_OUT_SIZE); n += ENC_GRU4_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_conv4, &buf[n], st->conv4_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_CONV4_OUT_SIZE;

    compute_generic_gru(&model->enc_gru5_input, &model->enc_gru5_recurrent, st->gru5_state, buf, arch);
    OPUS_COPY(&buf[n], st->gru5_state, ENC_GRU5_OUT_SIZE); n += ENC_GRU5_OUT_SIZE;
    compute_generic_conv1d_dilation(&model->enc_conv5, &buf[n], st->conv5_state, buf, n, 2, ACTIVATION_TANH, arch);
    n += ENC_CONV5_OUT_SIZE;

    /* z_dense: tanh-bounded latent only for bottleneck 1; linear otherwise. */
    int act = (bottleneck == 1) ? ACTIVATION_TANH : ACTIVATION_LINEAR;
    compute_generic_dense(&model->enc_zdense, z, buf, act, arch);
}
