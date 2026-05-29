/*---------------------------------------------------------------------------*\

  radc_dec.c

  Streaming neural decoder forward pass. Mirrors CoreDecoderStatefull.forward in
  radae/radae/radae_base.py (dense1 -> 5x (GRU -> GLU -> Conv1D) -> output),
  evaluated with the reused Opus DNN primitives. All decoder convs are dilation 1.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_dec.h"
#include "os_support.h"
#include <string.h>

void rade_init_decoder(RADEDecState *st) {
    memset(st, 0, sizeof(*st));
}

void rade_core_decoder(RADEDecState *st, const RADEDec *model,
                       float *features, const float *z_hat, int arch) {
    float buf[DEC_DENSE1_OUT_SIZE + DEC_GRU1_OUT_SIZE + DEC_CONV1_OUT_SIZE
            + DEC_GRU2_OUT_SIZE + DEC_CONV2_OUT_SIZE + DEC_GRU3_OUT_SIZE
            + DEC_CONV3_OUT_SIZE + DEC_GRU4_OUT_SIZE + DEC_CONV4_OUT_SIZE
            + DEC_GRU5_OUT_SIZE + DEC_CONV5_OUT_SIZE];
    int n = 0;

    compute_generic_dense(&model->dec_dense1, &buf[n], z_hat, ACTIVATION_TANH, arch);
    n += DEC_DENSE1_OUT_SIZE;

    compute_generic_gru(&model->dec_gru1_input, &model->dec_gru1_recurrent, st->gru1_state, buf, arch);
    compute_glu(&model->dec_glu1, &buf[n], st->gru1_state, arch); n += DEC_GRU1_OUT_SIZE;
    compute_generic_conv1d(&model->dec_conv1, &buf[n], st->conv1_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_CONV1_OUT_SIZE;

    compute_generic_gru(&model->dec_gru2_input, &model->dec_gru2_recurrent, st->gru2_state, buf, arch);
    compute_glu(&model->dec_glu2, &buf[n], st->gru2_state, arch); n += DEC_GRU2_OUT_SIZE;
    compute_generic_conv1d(&model->dec_conv2, &buf[n], st->conv2_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_CONV2_OUT_SIZE;

    compute_generic_gru(&model->dec_gru3_input, &model->dec_gru3_recurrent, st->gru3_state, buf, arch);
    compute_glu(&model->dec_glu3, &buf[n], st->gru3_state, arch); n += DEC_GRU3_OUT_SIZE;
    compute_generic_conv1d(&model->dec_conv3, &buf[n], st->conv3_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_CONV3_OUT_SIZE;

    compute_generic_gru(&model->dec_gru4_input, &model->dec_gru4_recurrent, st->gru4_state, buf, arch);
    compute_glu(&model->dec_glu4, &buf[n], st->gru4_state, arch); n += DEC_GRU4_OUT_SIZE;
    compute_generic_conv1d(&model->dec_conv4, &buf[n], st->conv4_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_CONV4_OUT_SIZE;

    compute_generic_gru(&model->dec_gru5_input, &model->dec_gru5_recurrent, st->gru5_state, buf, arch);
    compute_glu(&model->dec_glu5, &buf[n], st->gru5_state, arch); n += DEC_GRU5_OUT_SIZE;
    compute_generic_conv1d(&model->dec_conv5, &buf[n], st->conv5_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_CONV5_OUT_SIZE;

    compute_generic_dense(&model->dec_output, features, buf, ACTIVATION_LINEAR, arch);
}
