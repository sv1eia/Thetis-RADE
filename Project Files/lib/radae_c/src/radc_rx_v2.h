/*---------------------------------------------------------------------------*\

  radc_rx_v2.h

  RADE V2 receiver: the acquisition + frame-sync state machine that ties together
  AGC, CP-autocorrelation acquisition (radc_acq_v2), symbol extraction + demod
  (radc_demod_v2), the ML frame-sync net (radc_sync_v2), the neural decoder
  (radc_dec_v2), and EOO detection. Ported from RADEv2Receiver (radae/radae_v2.py)
  and the rx2.py driver loop.

  The caller drives it one symbol at a time: read rade_rx_v2_nin() samples, call
  rade_rx_v2_process(); it returns the number of decoded feature vectors (0, or
  frames_per_step=4 on a winning frame).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_RX_V2_H
#define RADC_RX_V2_H

#include "radc_modem_v2.h"
#include "radc_acq_v2.h"
#include "radc_demod_v2.h"
#include "radc_sync_v2.h"
#include "radc_dec_v2.h"
#include "radc_bpf.h"

#define RADC_V2_STATE_IDLE 0
#define RADC_V2_STATE_SYNC 1

typedef struct {
    radc_modem_v2  modem;
    radc_acq_v2    acq;            /* owns rx_buf, Ry_smooth, detection, SNR */
    radc_bpf       bpf;            /* input band-pass filter (shared complex_bpf) */
    int            bpf_en;
    RADESync       sync_model;
    RADEDecV2      dec_model;
    RADEDecV2State dec_state;

    /* Config (mirrors rx2.py args). */
    int   agc, fix_delta_hat, hangover, mute, limit_pitch, reset_output_on_resync;
    int   timing_adj_enable, verbose;
    int   time_offset, correct_time_offset;
    float agc_target;

    /* State machine. */
    int   state, count, count1, n_acq, s, i, timing_adj;
    float freq_offset, delta_hat, freq_offset_g;
    int   delta_hat_g, new_sig_delta_hat, new_sig_f_hat;
    float frame_sync_even, frame_sync_odd;

    /* Symbol extraction. */
    radc_cf rx_i[RADC_V2_NS * RADC_V2_SYM_LEN];
    radc_cf rx_phase;
    radc_cf rx_phase_vec[RADC_V2_SYM_LEN];
    radc_cf rx_sym_td[RADC_V2_M];
    float   z_hat_cur[RADC_V2_LATENT_DIM];

    /* EOO. */
    float eoo_smooth, eoo_corr;
    int   eoo_count;

    /* Last winning latent + status. */
    float az_hat[RADC_V2_LATENT_DIM];
    int   have_az_hat;
    float snr_est_db;
    int   eoo_detected;     /* set on the call that detects EOO (cleared each call) */

    int   nin;
} rade_rx_v2_state;

int   rade_rx_v2_init(rade_rx_v2_state *rx, int agc, int bpf_en);
int   rade_rx_v2_nin(const rade_rx_v2_state *rx);
int   rade_rx_v2_nin_max(const rade_rx_v2_state *rx);          /* sym_len + sym_len/4 */
int   rade_rx_v2_n_features_out(const rade_rx_v2_state *rx);   /* frames_per_step * num_features */
float rade_rx_v2_freq_offset(const rade_rx_v2_state *rx);

/* Process one symbol (rade_rx_v2_nin() complex samples). Returns the number of
   decoded feature vectors written to features_out (0, or frames_per_step). */
int   rade_rx_v2_process(rade_rx_v2_state *rx, float *features_out, const radc_cf *rx_in);

int   rade_rx_v2_sync(const rade_rx_v2_state *rx);
float rade_rx_v2_snrdB(const rade_rx_v2_state *rx);

#endif /* RADC_RX_V2_H */
