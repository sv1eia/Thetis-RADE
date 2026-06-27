/*---------------------------------------------------------------------------*\

  radc_enc_v2.h

  Run-time state for the streaming RADE V2 neural encoder (persisted GRU hidden
  states and Conv1D history). Sizes come from the generated rade_enc_v2_data.h.

  The V2 encoder topology is identical to V1's CoreEncoderStatefull (dense1 ->
  5x(GRU -> Conv1D) -> z_dense, convs dilation 1,2,2,2,2); only latent_dim (56)
  and the absence of a latent bottleneck differ. The dilation-2 convs (conv2..5)
  need 2*IN frames of history, as the generated *_STATE_SIZE macros assume
  dilation 1 (see radc_enc.h).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_ENC_V2_H
#define RADC_ENC_V2_H

#include "rade_v2_core.h"
#include "rade_enc_v2_data.h"

struct RADEEncV2Struct {
    int initialized;
    float gru1_state[ENC_V2_GRU1_STATE_SIZE];
    float gru2_state[ENC_V2_GRU2_STATE_SIZE];
    float gru3_state[ENC_V2_GRU3_STATE_SIZE];
    float gru4_state[ENC_V2_GRU4_STATE_SIZE];
    float gru5_state[ENC_V2_GRU5_STATE_SIZE];
    float conv1_state[ENC_V2_CONV1_IN_SIZE * 1];
    float conv2_state[ENC_V2_CONV2_IN_SIZE * 2];
    float conv3_state[ENC_V2_CONV3_IN_SIZE * 2];
    float conv4_state[ENC_V2_CONV4_IN_SIZE * 2];
    float conv5_state[ENC_V2_CONV5_IN_SIZE * 2];
};

#endif /* RADC_ENC_V2_H */
