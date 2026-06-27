/*---------------------------------------------------------------------------*\

  rade_v2_core.h

  Core neural interface for RADE V2: the encoder (RADEEncV2), decoder (RADEDecV2)
  and ML frame-sync (RADESync) model weight structs and the layer-size macros are
  emitted by the offline weight exporter (tools/export_rade_v2_weights.py) into
  rade_enc_v2_data.h / rade_dec_v2_data.h / rade_sync_data.h; the GRU/Conv
  run-time state structs (RADEEncV2State, RADEDecV2State) are defined in
  radc_enc_v2.h / radc_dec_v2.h.

  The forward passes (radc_enc_v2.c / radc_dec_v2.c / radc_sync_v2.c) mirror the
  layer wiring of the stateful V2 encoder/decoder in radae/radae/radae_base.py
  and the FrameSyncNet in radae/models_sync.py, evaluated with the reused Opus DNN
  primitives (compute_generic_*). V2 differs from V1 by: latent_dim 56, decoder
  hidden width 128, no latent bottleneck (peak=True), and the added frame-sync net.

  Analogue of rade_core.h (V1).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADE_V2_CORE_H
#define RADE_V2_CORE_H

#include "opus_types.h"
#include "nnet.h"

typedef struct RADEEncV2 RADEEncV2;             /* model, defined in rade_enc_v2_data.h */
typedef struct RADEDecV2 RADEDecV2;             /* model, defined in rade_dec_v2_data.h */
typedef struct RADESync  RADESync;              /* model, defined in rade_sync_data.h   */

typedef struct RADEEncV2Struct RADEEncV2State;  /* run-time state, radc_enc_v2.h */
typedef struct RADEDecV2Struct RADEDecV2State;  /* run-time state, radc_dec_v2.h */

/* Encoder: 4*21 features -> latent z[56]. No bottleneck (peak=True, bottleneck=0). */
void rade_init_encoder_v2(RADEEncV2State *st);
void rade_core_encoder_v2(RADEEncV2State *st, const RADEEncV2 *model,
                          float *z, const float *features, int arch);

/* Decoder: latent z_hat[56] -> 4*21 features. */
void rade_init_decoder_v2(RADEDecV2State *st);
void rade_core_decoder_v2(RADEDecV2State *st, const RADEDecV2 *model,
                          float *features, const float *z_hat, int arch);

/* ML frame sync: latent z_hat[56] -> sigmoid probability of frame alignment.
   Stateless (no recurrence), so no run-time state struct. */
float rade_frame_sync_v2(const RADESync *model, const float *z_hat, int arch);

extern const WeightArray radeencv2_arrays[];
extern const WeightArray radedecv2_arrays[];
extern const WeightArray radesync_arrays[];

#endif /* RADE_V2_CORE_H */
