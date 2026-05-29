/*---------------------------------------------------------------------------*\

  radc_tx.c   (port of class radae_tx / do_radae_tx, radae/radae_txe.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "rade_tx.h"
#include <string.h>

int rade_tx_init(rade_tx_state *tx, int bottleneck, int auxdata, int bpf_en) {
    memset(tx, 0, sizeof(*tx));
    tx->bottleneck = bottleneck;
    tx->auxdata = auxdata;
    tx->num_features = RADC_NUM_FEATURES + (auxdata ? 1 : 0);
    tx->bpf_en = bpf_en;
    tx->n_eoo_bits = RADC_NSEOO * 2;

    radc_modem_init(&tx->modem, bottleneck);
    if (init_radeenc(&tx->enc_model, radeenc_arrays) != 0) return -1;
    rade_init_encoder(&tx->enc_state);

    if (bpf_en) {
        float w0 = tx->modem.w[0], w1 = tx->modem.w[RADC_NC - 1];
        float bw = 1.2f * (w1 - w0) * RADC_FS / (2.0f * (float)M_PI);
        float ctr = (w1 + w0) * RADC_FS / (2.0f * (float)M_PI) / 2.0f;
        radc_bpf_init(&tx->bpf, RADC_BPF_NTAP, (float)RADC_FS, bw, ctr);
    }
    return 0;
}

int rade_tx_n_features_in(const rade_tx_state *tx) {
    (void)tx; return RADC_NZMF * RADC_FRAMES_PER_STEP * RADC_NB_TOTAL_FEATURES;
}
int rade_tx_n_samples_out(const rade_tx_state *tx) { (void)tx; return RADC_NMF; }
int rade_tx_n_eoo_out(const rade_tx_state *tx)     { (void)tx; return RADC_NEOO; }
int rade_tx_n_eoo_bits(const rade_tx_state *tx)    { return tx->n_eoo_bits; }

void rade_tx_state_set_eoo_bits(rade_tx_state *tx, const float *bits) {
    memcpy(tx->eoo_bits, bits, sizeof(float) * tx->n_eoo_bits);
}

int rade_tx_process(rade_tx_state *tx, RADE_COMP *tx_out, const float *features_in) {
    const int FPS = RADC_FRAMES_PER_STEP, NB = RADC_NB_TOTAL_FEATURES;
    float z[RADC_NZMF * RADC_LATENT_DIM];

    /* Per latent vector: assemble FPS frames of (20 used features + aux=-1),
       then run one stateful encoder step. */
    for (int k = 0; k < RADC_NZMF; k++) {
        float feats[RADC_FRAMES_PER_STEP * RADC_NUM_FEATURES_AUX];
        for (int i = 0; i < FPS; i++) {
            const float *src = &features_in[(k * FPS + i) * NB];
            float *dst = &feats[i * tx->num_features];
            for (int j = 0; j < RADC_NUM_FEATURES; j++) dst[j] = src[j];
            if (tx->auxdata) dst[RADC_NUM_FEATURES] = -1.0f;
        }
        rade_core_encoder(&tx->enc_state, &tx->enc_model,
                          &z[k * RADC_LATENT_DIM], feats, /*arch=*/0, tx->bottleneck);
    }

    radc_cf *out = (radc_cf *)tx_out;       /* RADE_COMP and radc_cf share layout */
    int n = radc_mod_frame(&tx->modem, out, z);

    if (tx->bpf_en) {
        radc_bpf_process(&tx->bpf, out, out, n);
        for (int i = 0; i < n; i++) {
            float mag = radc_cabs(out[i]);
            if (mag > 1.0f) out[i] = radc_cscale(out[i], 1.0f / mag);
        }
    }
    return n;
}

int rade_tx_state_eoo(rade_tx_state *tx, RADE_COMP *tx_out) {
    radc_cf *out = (radc_cf *)tx_out;
    int n = radc_mod_eoo(&tx->modem, out, tx->eoo_bits);
    if (tx->bpf_en) {
        radc_bpf_process(&tx->bpf, out, out, n);
        for (int i = 0; i < n; i++) {
            float mag = radc_cabs(out[i]);
            if (mag > 1.0f) out[i] = radc_cscale(out[i], 1.0f / mag);
        }
    }
    return n;
}
