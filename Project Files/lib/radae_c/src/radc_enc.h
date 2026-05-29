/*---------------------------------------------------------------------------*\

  radc_enc.h

  Run-time state for the streaming neural encoder (persisted GRU hidden states
  and Conv1D history). Sizes come from the generated rade_enc_data.h.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_ENC_H
#define RADC_ENC_H

#include "rade_core.h"
#include "rade_enc_data.h"

struct RADEEncStruct {
    int initialized;
    float gru1_state[ENC_GRU1_STATE_SIZE];
    float gru2_state[ENC_GRU2_STATE_SIZE];
    float gru3_state[ENC_GRU3_STATE_SIZE];
    float gru4_state[ENC_GRU4_STATE_SIZE];
    float gru5_state[ENC_GRU5_STATE_SIZE];
    /* Conv1D history. compute_generic_conv1d_dilation needs (ksize-1)*dilation
       input frames of memory; the generated *_STATE_SIZE macros assume dilation
       1, so the dilation-2 convs (conv2..5, per radae_base.py) need 2*IN. */
    float conv1_state[ENC_CONV1_IN_SIZE * 1];
    float conv2_state[ENC_CONV2_IN_SIZE * 2];
    float conv3_state[ENC_CONV3_IN_SIZE * 2];
    float conv4_state[ENC_CONV4_IN_SIZE * 2];
    float conv5_state[ENC_CONV5_IN_SIZE * 2];
};

#endif /* RADC_ENC_H */
