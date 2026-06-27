/*---------------------------------------------------------------------------*\

  radc_sync_v2.c

  RADE V2 ML frame-sync forward pass. Mirrors FrameSyncNet.forward in
  radae/models_sync.py: Linear(56->64) ReLU -> Linear(64->64) ReLU ->
  Linear(64->1) Sigmoid, evaluated with the reused Opus DNN primitives.
  Stateless.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_sync_v2.h"

float rade_frame_sync_v2(const RADESync *model, const float *z_hat, int arch) {
    float h1[SYNC_DENSE1_OUT_SIZE];
    float h2[SYNC_DENSE2_OUT_SIZE];
    float y[SYNC_DENSE3_OUT_SIZE];

    compute_generic_dense(&model->sync_dense1, h1, z_hat, ACTIVATION_RELU, arch);
    compute_generic_dense(&model->sync_dense2, h2, h1,    ACTIVATION_RELU, arch);
    compute_generic_dense(&model->sync_dense3, y,  h2,    ACTIVATION_SIGMOID, arch);
    return y[0];
}
