/*---------------------------------------------------------------------------*\

  radc_tx_v2.c   (port of tx2.py / RADEv2Transmitter, radae/radae_v2.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_tx_v2.h"
#include <string.h>

int rade_tx_v2_init(rade_tx_v2_state *tx, int auxdata) {
    memset(tx, 0, sizeof(*tx));
    tx->auxdata = auxdata;
    tx->num_features = RADC_V2_NUM_FEATURES + (auxdata ? 1 : 0);

    radc_modem_v2_init(&tx->modem);
    if (init_radeencv2(&tx->enc_model, radeencv2_arrays) != 0) return -1;
    rade_init_encoder_v2(&tx->enc_state);
    return 0;
}

int rade_tx_v2_n_features_in(const rade_tx_v2_state *tx) {
    (void)tx; return RADC_V2_NZMF * RADC_V2_FRAMES_PER_STEP * RADC_V2_NB_TOTAL_FEATURES;
}
int rade_tx_v2_n_samples_out(const rade_tx_v2_state *tx) { (void)tx; return RADC_V2_NMF; }
int rade_tx_v2_n_eoo_out(const rade_tx_v2_state *tx)     { (void)tx; return RADC_V2_NEOO; }

int rade_tx_v2_process(rade_tx_v2_state *tx, RADE_COMP *tx_out, const float *features_in) {
    const int FPS = RADC_V2_FRAMES_PER_STEP, NB = RADC_V2_NB_TOTAL_FEATURES;
    float z[RADC_V2_LATENT_DIM];

    /* One modem frame (Nzmf=1): assemble FPS frames of (20 used features + aux=-1),
       then run one stateful V2 encoder step. */
    float feats[RADC_V2_FRAMES_PER_STEP * RADC_V2_NUM_FEATURES_AUX];
    for (int i = 0; i < FPS; i++) {
        const float *src = &features_in[i * NB];
        float *dst = &feats[i * tx->num_features];
        for (int j = 0; j < RADC_V2_NUM_FEATURES; j++) dst[j] = src[j];
        if (tx->auxdata) dst[RADC_V2_NUM_FEATURES] = -1.0f;
    }
    rade_core_encoder_v2(&tx->enc_state, &tx->enc_model, z, feats, /*arch=*/0);

    radc_cf *out = (radc_cf *)tx_out;       /* RADE_COMP and radc_cf share layout */
    return radc_mod_v2_frame(&tx->modem, out, z);
}

int rade_tx_v2_eoo(rade_tx_v2_state *tx, RADE_COMP *tx_out) {
    return radc_mod_v2_eoo(&tx->modem, (radc_cf *)tx_out);
}
