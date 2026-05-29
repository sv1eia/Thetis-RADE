/*---------------------------------------------------------------------------*\

  rade_tx.h

  Transmitter pipeline state + entry points. Ported from radae/radae_txe.py
  (class radae_tx / do_radae_tx): features -> neural encoder -> OFDM modulator.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADE_TX_H
#define RADE_TX_H

#include "radc_modem.h"
#include "radc_mod.h"
#include "radc_bpf.h"
#include "radc_enc.h"

typedef struct {
    radc_modem modem;
    RADEEnc    enc_model;
    RADEEncState enc_state;
    radc_bpf   bpf;
    int        bpf_en;
    int        bottleneck;
    int        auxdata;
    int        num_features;        /* 20, or 21 with auxdata */
    float      eoo_bits[RADC_NSEOO * 2];
    int        n_eoo_bits;
} rade_tx_state;

int  rade_tx_init(rade_tx_state *tx, int bottleneck, int auxdata, int bpf_en);
int  rade_tx_n_features_in(const rade_tx_state *tx);   /* floats consumed per frame */
int  rade_tx_n_samples_out(const rade_tx_state *tx);   /* Nmf */
int  rade_tx_n_eoo_out(const rade_tx_state *tx);       /* Neoo */
int  rade_tx_n_eoo_bits(const rade_tx_state *tx);
void rade_tx_state_set_eoo_bits(rade_tx_state *tx, const float *bits);

int  rade_tx_process(rade_tx_state *tx, RADE_COMP *tx_out, const float *features_in);
int  rade_tx_state_eoo(rade_tx_state *tx, RADE_COMP *tx_out);

#endif /* RADE_TX_H */
