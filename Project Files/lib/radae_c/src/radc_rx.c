/*---------------------------------------------------------------------------*\

  radc_rx.c   (port of class radae_rx / do_radae_rx, radae/radae_rxe.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "rade_rx.h"
#include <string.h>
#include <math.h>

int rade_rx_init(rade_rx_state *rx, int bottleneck, int auxdata, int bpf_en) {
    memset(rx, 0, sizeof(*rx));
    rx->bottleneck = bottleneck;
    rx->auxdata = auxdata;
    rx->num_features = RADC_NUM_FEATURES + (auxdata ? 1 : 0);
    rx->coarse_mag = 1;
    rx->time_offset = RADC_TIME_OFFSET;
    rx->verbose = 0;
    rx->bpf_en = bpf_en;

    radc_modem_init(&rx->modem, bottleneck);
    radc_acq_init(&rx->acq, &rx->modem, RADC_ACQ_FRANGE, RADC_ACQ_FSTEP, /*seed=*/0);
    if (init_radedec(&rx->dec_model, radedec_arrays) != 0) return -1;
    rade_init_decoder(&rx->dec_state);

    if (bpf_en) {
        float w0 = rx->modem.w[0], w1 = rx->modem.w[RADC_NC - 1];
        float bw = 1.2f * (w1 - w0) * RADC_FS / (2.0f * (float)M_PI);
        float ctr = (w1 + w0) * RADC_FS / (2.0f * (float)M_PI) / 2.0f;
        radc_bpf_init(&rx->bpf, RADC_BPF_NTAP, (float)RADC_FS, bw, ctr);
    }

    rx->state = RADC_STATE_SEARCH;
    rx->nin = RADC_NMF;
    rx->rx_phase = radc_cone();
    rx->Nmf_unsync = (int)(RADC_TUNSYNC * RADC_FS / RADC_NMF);
    rx->synced_count_one_sec = RADC_FS / RADC_NMF;
    return 0;
}

int rade_rx_nin(const rade_rx_state *rx)     { return rx->nin; }
int rade_rx_nin_max(const rade_rx_state *rx) { (void)rx; return RADC_NMF + RADC_M; }
int rade_rx_n_features_out(const rade_rx_state *rx) {
    (void)rx; return RADC_NZMF * RADC_FRAMES_PER_STEP * RADC_NB_TOTAL_FEATURES;
}
int rade_rx_n_eoo_bits(const rade_rx_state *rx) { (void)rx; return RADC_NSEOO * 2; }
int rade_rx_sync(const rade_rx_state *rx) { return rx->state == RADC_STATE_SYNC; }
float rade_rx_snrdB_3k_est(const rade_rx_state *rx) { return rx->snrdB_3k_est; }
float rade_rx_freq_offset(const rade_rx_state *rx)  { return rx->fmax; }

int rade_rx_process(rade_rx_state *rx, float *features_out, float *eoo_out, const RADE_COMP *rx_in) {
    const int M = RADC_M, Ncp = RADC_NCP, Nmf = RADC_NMF;
    const float Fs = (float)RADC_FS;
    const int buf = RADC_RX_BUF_SIZE;

    int candidate = 0, endofover = 0, uw_fail = 0, valid_output = 0;

    /* Optional input BPF. */
    radc_cf filt[RADC_NMF + RADC_M];
    const radc_cf *samples = (const radc_cf *)rx_in;
    if (rx->bpf_en) { radc_bpf_process(&rx->bpf, filt, (const radc_cf *)rx_in, rx->nin); samples = filt; }

    /* Slide receive buffer: out with the old, in with the new. */
    if (rx->nin > 0) {
        memmove(rx->rx_buf, &rx->rx_buf[rx->nin], sizeof(radc_cf) * (buf - rx->nin));
        memcpy(&rx->rx_buf[buf - rx->nin], samples, sizeof(radc_cf) * rx->nin);
    }

    if (rx->state == RADC_STATE_SEARCH || rx->state == RADC_STATE_CANDIDATE) {
        candidate = radc_acq_detect(&rx->acq, rx->rx_buf, &rx->tmax, &rx->fmax);
    } else {
        /* SYNC: refine, low-pass freq, spot-check pilots. */
        int t0 = (rx->tmax > 8) ? rx->tmax - 8 : 0;
        float fhat = rx->fmax;
        radc_acq_refine(&rx->acq, rx->rx_buf, &rx->tmax, &fhat,
                        t0, rx->tmax + 8, rx->fmax - 1.0f, rx->fmax + 1.0f, 0.1f);
        rx->fmax = 0.9f * rx->fmax + 0.1f * fhat;
        radc_acq_check(&rx->acq, rx->rx_buf, rx->tmax, rx->fmax, &candidate, &endofover);

        /* Timing-slip handling via nin. */
        rx->nin = Nmf;
        if (rx->tmax >= Nmf - M) { rx->nin = Nmf + M; rx->tmax -= M; }
        if (rx->tmax < M)        { rx->nin = Nmf - M; rx->tmax += M; }

        rx->synced_count++;
        if (rx->synced_count % rx->synced_count_one_sec == 0) {
            if (rx->uw_errors > RADC_UW_ERR_THRESH) uw_fail = 1;
            rx->uw_errors = 0;
        }

        /* De-rotate by the tracked frequency offset (phase carried across frames). */
        float w = 2.0f * (float)M_PI * rx->fmax / Fs;
        radc_cf corr[RADC_DEMOD_IN_LEN];
        radc_cf ph = rx->rx_phase;
        radc_cf step = radc_cexpj(-w);
        for (int n = 0; n < Nmf + M + Ncp; n++) {
            ph = radc_cmul(ph, step);
            corr[n] = radc_cmul(rx->rx_buf[rx->tmax - Ncp + n], ph);
        }
        rx->rx_phase = ph;

        if (!endofover) {
            float z_hat[RADC_NZMF * RADC_LATENT_DIM];
            float snr = radc_demod_frame(&rx->modem, z_hat, corr, rx->time_offset, rx->coarse_mag);
            rx->snrdB_3k_est = 0.9f * rx->snrdB_3k_est + 0.1f * snr;
            valid_output = 1;

            /* Decode each latent vector to FRAMES_PER_STEP feature frames. */
            int nout = rade_rx_n_features_out(rx);
            memset(features_out, 0, sizeof(float) * nout);
            int uw = 0;
            for (int k = 0; k < RADC_NZMF; k++) {
                float dec[RADC_FRAMES_PER_STEP * RADC_NUM_FEATURES_AUX];
                rade_core_decoder(&rx->dec_state, &rx->dec_model, dec,
                                  &z_hat[k * RADC_LATENT_DIM], /*arch=*/0);
                for (int i = 0; i < RADC_FRAMES_PER_STEP; i++) {
                    int o = (k * RADC_FRAMES_PER_STEP + i) * RADC_NB_TOTAL_FEATURES;
                    for (int j = 0; j < RADC_NUM_FEATURES; j++)
                        features_out[o + j] = dec[i * rx->num_features + j];
                }
                if (rx->auxdata && dec[RADC_NUM_FEATURES] > 0.0f) uw++;  /* aux symbol */
            }
            if (rx->auxdata) rx->uw_errors += uw;
        } else {
            radc_demod_eoo(&rx->modem, eoo_out, corr, rx->time_offset);
        }
    }

    /* State machine transitions (search -> candidate -> sync; unsync logic). */
    int next = rx->state;
    if (rx->state == RADC_STATE_SEARCH) {
        if (candidate) { next = RADC_STATE_CANDIDATE; rx->tmax_candidate = rx->tmax; rx->valid_count = 1; }
    } else if (rx->state == RADC_STATE_CANDIDATE) {
        if (candidate && abs(rx->tmax - rx->tmax_candidate) < Ncp) {
            if (++rx->valid_count > 3) {
                next = RADC_STATE_SYNC;
                rade_init_decoder(&rx->dec_state);
                rx->synced_count = 0; rx->uw_errors = 0;
                rx->valid_count = rx->Nmf_unsync;
                int t0 = (rx->tmax > 1) ? rx->tmax - 1 : 0;
                radc_acq_refine(&rx->acq, rx->rx_buf, &rx->tmax, &rx->fmax,
                                t0, rx->tmax + 2, rx->fmax - 10.0f, rx->fmax + 10.0f, 0.25f);
            }
        } else {
            next = RADC_STATE_SEARCH;
        }
    } else { /* SYNC */
        int unsync_enable = 1;
        if (rx->disable_unsync > 0.0f &&
            rx->synced_count > (int)(rx->disable_unsync * Fs / Nmf)) unsync_enable = 0;
        if (candidate) rx->valid_count = rx->Nmf_unsync;
        else if (unsync_enable && --rx->valid_count == 0) next = RADC_STATE_SEARCH;
        if (unsync_enable && (endofover || uw_fail)) next = RADC_STATE_SEARCH;
    }
    rx->state = next;
    if (rx->state == RADC_STATE_SEARCH) rx->nin = Nmf;
    rx->mf++;

    return (valid_output ? 0x1 : 0) | (endofover ? 0x2 : 0);
}
