/*---------------------------------------------------------------------------*\

  radc_dec_v2.c

  Streaming RADE V2 neural decoder forward pass. Mirrors CoreDecoderStatefull.forward
  in radae/radae/radae_base.py (dense1 -> 5x (GRU -> GLU -> Conv1D) -> output),
  evaluated with the reused Opus DNN primitives. All decoder convs are dilation 1.

  V2 vs V1: hidden width w1=128 (not 96); the output dense is still linear and
  produces frames_per_step*num_features = 4*21 = 84.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_dec_v2.h"
#include "os_support.h"
#include <string.h>

/* Local dilation-1 conv1d with scratch sized to the V2 decoder's widest conv.
   Opus's compute_generic_conv1d uses a fixed stack buffer tmp[DRED_MAX_CONV_INPUTS]
   (=1536), but a kernel-2 Conv1d has nb_inputs = 2*in_channels, and the V2
   decoder conv5 needs 2*896 = 1792 = RADE_V2_DEC_MAX_CONV_INPUTS, which overflows
   it (V1's max is 1408, the V2 encoder's is exactly 1536). We reuse the Opus
   primitives compute_linear/compute_activation with a correctly sized buffer; the
   math is byte-identical to compute_generic_conv1d. */
static void conv1d_v2(const LinearLayer *layer, float *output, float *mem,
                      const float *input, int input_size, int activation, int arch) {
    float tmp[RADE_V2_DEC_MAX_CONV_INPUTS];
    int ni = layer->nb_inputs;
    if (ni != input_size) OPUS_COPY(tmp, mem, ni - input_size);
    OPUS_COPY(&tmp[ni - input_size], input, input_size);
    compute_linear(layer, output, tmp, arch);
    compute_activation(output, output, layer->nb_outputs, activation, arch);
    if (ni != input_size) OPUS_COPY(mem, &tmp[input_size], ni - input_size);
}

void rade_init_decoder_v2(RADEDecV2State *st) {
    memset(st, 0, sizeof(*st));
}

void rade_core_decoder_v2(RADEDecV2State *st, const RADEDecV2 *model,
                          float *features, const float *z_hat, int arch) {
    float buf[DEC_V2_DENSE1_OUT_SIZE + DEC_V2_GRU1_OUT_SIZE + DEC_V2_CONV1_OUT_SIZE
            + DEC_V2_GRU2_OUT_SIZE + DEC_V2_CONV2_OUT_SIZE + DEC_V2_GRU3_OUT_SIZE
            + DEC_V2_CONV3_OUT_SIZE + DEC_V2_GRU4_OUT_SIZE + DEC_V2_CONV4_OUT_SIZE
            + DEC_V2_GRU5_OUT_SIZE + DEC_V2_CONV5_OUT_SIZE];
    int n = 0;

    compute_generic_dense(&model->dec_v2_dense1, &buf[n], z_hat, ACTIVATION_TANH, arch);
    n += DEC_V2_DENSE1_OUT_SIZE;

    compute_generic_gru(&model->dec_v2_gru1_input, &model->dec_v2_gru1_recurrent, st->gru1_state, buf, arch);
    compute_glu(&model->dec_v2_glu1, &buf[n], st->gru1_state, arch); n += DEC_V2_GRU1_OUT_SIZE;
    conv1d_v2(&model->dec_v2_conv1, &buf[n], st->conv1_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_V2_CONV1_OUT_SIZE;

    compute_generic_gru(&model->dec_v2_gru2_input, &model->dec_v2_gru2_recurrent, st->gru2_state, buf, arch);
    compute_glu(&model->dec_v2_glu2, &buf[n], st->gru2_state, arch); n += DEC_V2_GRU2_OUT_SIZE;
    conv1d_v2(&model->dec_v2_conv2, &buf[n], st->conv2_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_V2_CONV2_OUT_SIZE;

    compute_generic_gru(&model->dec_v2_gru3_input, &model->dec_v2_gru3_recurrent, st->gru3_state, buf, arch);
    compute_glu(&model->dec_v2_glu3, &buf[n], st->gru3_state, arch); n += DEC_V2_GRU3_OUT_SIZE;
    conv1d_v2(&model->dec_v2_conv3, &buf[n], st->conv3_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_V2_CONV3_OUT_SIZE;

    compute_generic_gru(&model->dec_v2_gru4_input, &model->dec_v2_gru4_recurrent, st->gru4_state, buf, arch);
    compute_glu(&model->dec_v2_glu4, &buf[n], st->gru4_state, arch); n += DEC_V2_GRU4_OUT_SIZE;
    conv1d_v2(&model->dec_v2_conv4, &buf[n], st->conv4_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_V2_CONV4_OUT_SIZE;

    compute_generic_gru(&model->dec_v2_gru5_input, &model->dec_v2_gru5_recurrent, st->gru5_state, buf, arch);
    compute_glu(&model->dec_v2_glu5, &buf[n], st->gru5_state, arch); n += DEC_V2_GRU5_OUT_SIZE;
    conv1d_v2(&model->dec_v2_conv5, &buf[n], st->conv5_state, buf, n, ACTIVATION_TANH, arch);
    n += DEC_V2_CONV5_OUT_SIZE;

    compute_generic_dense(&model->dec_v2_output, features, buf, ACTIVATION_LINEAR, arch);
}
