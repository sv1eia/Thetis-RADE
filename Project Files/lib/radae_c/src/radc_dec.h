/*---------------------------------------------------------------------------*\

  radc_dec.h

  Run-time state for the streaming neural decoder (persisted GRU hidden states
  and Conv1D history). Sizes come from the generated rade_dec_data.h.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_DEC_H
#define RADC_DEC_H

#include "rade_core.h"
#include "rade_dec_data.h"

struct RADEDecStruct {
    int initialized;
    float gru1_state[DEC_GRU1_STATE_SIZE];
    float gru2_state[DEC_GRU2_STATE_SIZE];
    float gru3_state[DEC_GRU3_STATE_SIZE];
    float gru4_state[DEC_GRU4_STATE_SIZE];
    float gru5_state[DEC_GRU5_STATE_SIZE];
    float conv1_state[DEC_CONV1_STATE_SIZE];
    float conv2_state[DEC_CONV2_STATE_SIZE];
    float conv3_state[DEC_CONV3_STATE_SIZE];
    float conv4_state[DEC_CONV4_STATE_SIZE];
    float conv5_state[DEC_CONV5_STATE_SIZE];
};

#endif /* RADC_DEC_H */
