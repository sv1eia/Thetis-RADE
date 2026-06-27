/*---------------------------------------------------------------------------*\

  radc_dec_v2.h

  Run-time state for the streaming RADE V2 neural decoder (persisted GRU hidden
  states and Conv1D history). Sizes come from the generated rade_dec_v2_data.h.

  The V2 decoder topology is identical to V1's CoreDecoderStatefull (dense1 ->
  5x(GRU -> GLU -> Conv1D) -> output, all convs dilation 1); only the hidden
  width w1=128 (V1: 96) differs. rade_init_decoder_v2() zeros the state and also
  serves as the reference's reset() (called on receiver re-sync).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_DEC_V2_H
#define RADC_DEC_V2_H

#include "rade_v2_core.h"
#include "rade_dec_v2_data.h"

struct RADEDecV2Struct {
    int initialized;
    float gru1_state[DEC_V2_GRU1_STATE_SIZE];
    float gru2_state[DEC_V2_GRU2_STATE_SIZE];
    float gru3_state[DEC_V2_GRU3_STATE_SIZE];
    float gru4_state[DEC_V2_GRU4_STATE_SIZE];
    float gru5_state[DEC_V2_GRU5_STATE_SIZE];
    float conv1_state[DEC_V2_CONV1_STATE_SIZE];
    float conv2_state[DEC_V2_CONV2_STATE_SIZE];
    float conv3_state[DEC_V2_CONV3_STATE_SIZE];
    float conv4_state[DEC_V2_CONV4_STATE_SIZE];
    float conv5_state[DEC_V2_CONV5_STATE_SIZE];
};

#endif /* RADC_DEC_V2_H */
