/*---------------------------------------------------------------------------*\

  rade_core.h

  Core neural encoder/decoder interface. The model weight structs (RADEEnc,
  RADEDec) and the layer-size macros are emitted by the offline weight exporter
  into rade_enc_data.h / rade_dec_data.h; the GRU/Conv run-time state structs
  (RADEEncState, RADEDecState) are defined in radc_enc.h / radc_dec.h.

  The forward passes (radc_enc.c / radc_dec.c) mirror the layer wiring of the
  stateful encoder/decoder in radae/radae/radae_base.py, evaluated with the
  reused Opus DNN primitives (compute_generic_*).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADE_CORE_H
#define RADE_CORE_H

#include "opus_types.h"
#include "nnet.h"

typedef struct RADEEnc RADEEnc;             /* model, defined in rade_enc_data.h */
typedef struct RADEDec RADEDec;             /* model, defined in rade_dec_data.h */
typedef struct RADEEncStruct RADEEncState;  /* run-time state, radc_enc.h */
typedef struct RADEDecStruct RADEDecState;  /* run-time state, radc_dec.h */

void rade_init_encoder(RADEEncState *st);
void rade_core_encoder(RADEEncState *st, const RADEEnc *model,
                       float *z, const float *features, int arch, int bottleneck);

void rade_init_decoder(RADEDecState *st);
void rade_core_decoder(RADEDecState *st, const RADEDec *model,
                       float *features, const float *z_hat, int arch);

extern const WeightArray radeenc_arrays[];
extern const WeightArray radedec_arrays[];

#endif /* RADE_CORE_H */
