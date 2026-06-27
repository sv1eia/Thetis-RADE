/*---------------------------------------------------------------------------*\

  radc_tx_v2.h

  RADE V2 transmitter pipeline state + entry points. Ported from tx2.py /
  RADEv2Transmitter (radae/radae_v2.py): features -> stateful V2 neural encoder
  -> OFDM modulator. One modem frame = Nzmf(1) * frames_per_step(4) feature
  vectors in, RADC_V2_NMF (320) IQ samples out.

  The optional streaming SSB band-pass filter (model.ssb_bpf) is off by default in
  the V2 reference and is not yet wired here.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_TX_V2_H
#define RADC_TX_V2_H

#include "radc_modem_v2.h"   /* radc_cf, RADE_COMP (via radc_types.h) */
#include "radc_mod_v2.h"
#include "radc_enc_v2.h"

typedef struct {
    radc_modem_v2 modem;
    RADEEncV2     enc_model;
    RADEEncV2State enc_state;
    int           auxdata;
    int           num_features;   /* 20, or 21 with auxdata */
} rade_tx_v2_state;

int rade_tx_v2_init(rade_tx_v2_state *tx, int auxdata);
int rade_tx_v2_n_features_in(const rade_tx_v2_state *tx);   /* floats consumed per modem frame (144) */
int rade_tx_v2_n_samples_out(const rade_tx_v2_state *tx);   /* Nmf (320) */
int rade_tx_v2_n_eoo_out(const rade_tx_v2_state *tx);       /* Neoo (960) */

int rade_tx_v2_process(rade_tx_v2_state *tx, RADE_COMP *tx_out, const float *features_in);
int rade_tx_v2_eoo(rade_tx_v2_state *tx, RADE_COMP *tx_out);

#endif /* RADC_TX_V2_H */
