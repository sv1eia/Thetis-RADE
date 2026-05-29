/*---------------------------------------------------------------------------*\

  rade_rx.h

  Receiver pipeline state + entry points. Ported from radae/radae_rxe.py
  (class radae_rx / do_radae_rx): BPF -> acquisition/sync state machine ->
  frequency correction -> OFDM demod -> neural decoder, plus EOO handling.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADE_RX_H
#define RADE_RX_H

#include "radc_modem.h"
#include "radc_demod.h"
#include "radc_acq.h"
#include "radc_bpf.h"
#include "radc_dec.h"

typedef struct {
    radc_modem modem;
    radc_acq   acq;
    RADEDec    dec_model;
    RADEDecState dec_state;
    radc_bpf   bpf;
    int        bpf_en;

    int        bottleneck, auxdata, num_features, coarse_mag, time_offset, verbose;

    /* sync state machine */
    int        state, valid_count, synced_count, uw_errors;
    int        Nmf_unsync, synced_count_one_sec;
    int        tmax, tmax_candidate;
    float      fmax;
    radc_cf    rx_phase;
    int        nin;

    radc_cf    rx_buf[RADC_RX_BUF_SIZE];
    float      snrdB_3k_est;
    int        mf;
    float      disable_unsync;
} rade_rx_state;

int  rade_rx_init(rade_rx_state *rx, int bottleneck, int auxdata, int bpf_en);
int  rade_rx_nin(const rade_rx_state *rx);
int  rade_rx_nin_max(const rade_rx_state *rx);
int  rade_rx_n_features_out(const rade_rx_state *rx);
int  rade_rx_n_eoo_bits(const rade_rx_state *rx);
int  rade_rx_sync(const rade_rx_state *rx);
float rade_rx_snrdB_3k_est(const rade_rx_state *rx);
float rade_rx_freq_offset(const rade_rx_state *rx);

/* Returns flags: 0x1 valid features_out, 0x2 EOO (eoo_out filled). */
int  rade_rx_process(rade_rx_state *rx, float *features_out, float *eoo_out, const RADE_COMP *rx_in);

#endif /* RADE_RX_H */
