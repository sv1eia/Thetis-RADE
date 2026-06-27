/*---------------------------------------------------------------------------*\

  radc_acq_v2.h

  RADE V2 acquisition: cyclic-prefix autocorrelation for timing/frequency
  estimation, signal & sine-wave detection, and a CP-autocorr-peak SNR estimator.
  V2 has no pilots, so all of this is blind (CP-based). Ported from
  RADEv2Receiver._compute_autocorr / _detect_signal (radae/radae_v2.py).

  This module is the pure per-symbol acquisition front-end; the idle/sync state
  machine that consumes delta_hat_g / sig_det / freq_offset is added in the
  receiver (Phase 7).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_ACQ_V2_H
#define RADC_ACQ_V2_H

#include "radc_modem_v2.h"

typedef struct {
    int   m, ncp, ns, sym_len;
    float fs;
    float alpha, tsig, tsin;

    /* SNR-from-autocorr-peak */
    float b_bpf, snr_offset_db, snr_corr_a, snr_corr_b, snr_est_db;

    /* Sliding receive buffer (3 symbols) and autocorrelation state. */
    radc_cf rx_buf[3 * RADC_V2_SYM_LEN];
    radc_cf Ry_norm[RADC_V2_SYM_LEN];
    radc_cf Ry_smooth[RADC_V2_SYM_LEN];

    /* Per-symbol detection outputs. */
    int   delta_hat_g;          /* argmax |Ry_smooth| */
    float Ry_max, Ry_min;
    float freq_offset_inst;     /* -angle(Ry_smooth[delta_hat_g]) * Fs / (2*pi*M) */
} radc_acq_v2;

void radc_acq_v2_init(radc_acq_v2 *a, const radc_modem_v2 *m);

/* Slide nin new samples (scaled by gain) into the receive buffer. */
void radc_acq_v2_slide(radc_acq_v2 *a, const radc_cf *rx_in, int nin, float gain);

/* CP autocorrelation over all lags -> Ry_norm, then IIR -> Ry_smooth. */
void radc_acq_v2_autocorr(radc_acq_v2 *a);

/* Detection from Ry_smooth: sets delta_hat_g, Ry_max/min, snr_est_db,
   freq_offset_inst; returns sig_det / sine_det via the out pointers. */
void radc_acq_v2_detect(radc_acq_v2 *a, int *sig_det, int *sine_det);

#endif /* RADC_ACQ_V2_H */
