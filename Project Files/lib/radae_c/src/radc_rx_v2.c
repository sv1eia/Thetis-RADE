/*---------------------------------------------------------------------------*\

  radc_rx_v2.c   (port of RADEv2Receiver + rx2.py driver, radae/radae_v2.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_rx_v2.h"
#include <math.h>
#include <string.h>

#define FS_F  ((float)RADC_V2_FS)

int rade_rx_v2_init(rade_rx_v2_state *rx, int agc, int bpf_en) {
    memset(rx, 0, sizeof(*rx));
    radc_modem_v2_init(&rx->modem);
    radc_acq_v2_init(&rx->acq, &rx->modem);
    if (init_radesync(&rx->sync_model, radesync_arrays) != 0) return -1;
    if (init_radedecv2(&rx->dec_model, radedecv2_arrays) != 0) return -1;
    rade_init_decoder_v2(&rx->dec_state);

    /* Input BPF (same complex_bpf as V1 / rx2.py), sized to the V2 carriers. */
    rx->bpf_en = bpf_en;
    if (bpf_en) {
        float w0 = rx->modem.w[0], wN = rx->modem.w[rx->modem.nc - 1];
        float bw  = 1.2f * (wN - w0) * FS_F / (2.0f * (float)M_PI);
        float ctr = (wN + w0) * FS_F / (2.0f * (float)M_PI) / 2.0f;
        radc_bpf_init(&rx->bpf, RADC_BPF_NTAP, FS_F, bw, ctr);
    }

    rx->agc = agc;
    rx->fix_delta_hat = 0;
    rx->hangover = RADC_V2_HANGOVER;
    rx->mute = 0;
    rx->limit_pitch = 1;
    rx->reset_output_on_resync = 0;
    rx->timing_adj_enable = 0;     /* enable for long-run / clock-drift operation */
    rx->verbose = 0;
    rx->time_offset = RADC_V2_TIME_OFFSET;
    rx->correct_time_offset = RADC_V2_CORRECT_TIME_OFFSET;
    rx->agc_target = (float)pow(10.0, RADC_V2_AGC_TARGET_DB / 20.0);

    rx->state = RADC_V2_STATE_IDLE;
    rx->rx_phase = radc_cone();
    rx->nin = rx->acq.sym_len;
    return 0;
}

int rade_rx_v2_nin(const rade_rx_v2_state *rx) { return rx->nin; }
int rade_rx_v2_nin_max(const rade_rx_v2_state *rx) { return rx->acq.sym_len + rx->acq.sym_len / 4; }
int rade_rx_v2_n_features_out(const rade_rx_v2_state *rx) {
    (void)rx; return RADC_V2_FRAMES_PER_STEP * RADC_V2_NUM_FEATURES_AUX;
}
int   rade_rx_v2_sync(const rade_rx_v2_state *rx)  { return rx->state == RADC_V2_STATE_SYNC; }
float rade_rx_v2_snrdB(const rade_rx_v2_state *rx) { return rx->snr_est_db; }
float rade_rx_v2_freq_offset(const rade_rx_v2_state *rx) { return rx->freq_offset; }

static float compute_gain(const rade_rx_v2_state *rx, const radc_cf *rx_in, int nin) {
    if (!rx->agc) return 1.0f;
    float ms = 0.0f;
    for (int i = 0; i < nin; i++) ms += radc_cabs2(rx_in[i]);
    ms /= (float)nin;
    float g = rx->agc_target / (sqrtf(ms) + 1e-6f);
    if (g < 0.1f) g = 0.1f;
    if (g > 10.0f) g = 10.0f;
    return g;
}

static int process_idle(rade_rx_v2_state *rx, int sig_det, int sine_det) {
    const int M = rx->modem.m;
    if (sig_det && !sine_det) rx->count++;
    else rx->count = 0;

    if (rx->count == 5) {
        float dphi = radc_carg(rx->acq.Ry_smooth[rx->delta_hat_g]);
        rx->delta_hat   = (float)rx->delta_hat_g;
        rx->freq_offset = -dphi * FS_F / (2.0f * (float)M_PI * (float)M);
        rx->count = 0; rx->count1 = 0;
        rx->frame_sync_even = 0.0f; rx->frame_sync_odd = 0.0f;
        rx->eoo_smooth = 0.0f;
        if (rx->reset_output_on_resync) rx->i = 0;
        rx->n_acq++;
        return RADC_V2_STATE_SYNC;
    }
    return RADC_V2_STATE_IDLE;
}

static void extract_symbol(rade_rx_v2_state *rx) {
    const int M = rx->modem.m, Ncp = rx->modem.ncp, sym_len = rx->acq.sym_len;
    int delta_hat_rx = (int)(rx->delta_hat - (float)Ncp);
    float omega = 2.0f * (float)M_PI * rx->freq_offset / FS_F;
    radc_cf rot = radc_cexpj(-omega);
    for (int n = 0; n < sym_len; n++) {
        rx->rx_phase = radc_cmul(rx->rx_phase, rot);
        rx->rx_phase_vec[n] = rx->rx_phase;
    }
    int st = sym_len + delta_hat_rx;

    /* rx_i: shift down one symbol, append the new freq-corrected symbol. */
    memmove(rx->rx_i, &rx->rx_i[sym_len], sizeof(radc_cf) * (size_t)sym_len);
    for (int n = 0; n < sym_len; n++)
        rx->rx_i[sym_len + n] = radc_cmul(rx->rx_phase_vec[n], rx->acq.rx_buf[st + n]);
    /* rx_sym_td = (freq-corrected symbol)[Ncp:]  (M samples, for EOO). */
    for (int n = 0; n < M; n++)
        rx->rx_sym_td[n] = radc_cmul(rx->rx_phase_vec[Ncp + n], rx->acq.rx_buf[st + Ncp + n]);

    radc_demod_v2_frame(&rx->modem, rx->z_hat_cur, rx->rx_i,
                        rx->time_offset, rx->correct_time_offset);
}

static int detect_eoo(rade_rx_v2_state *rx) {
    rx->eoo_corr = radc_demod_v2_eoo_corr(&rx->modem, rx->rx_sym_td);
    rx->eoo_smooth = RADC_V2_ALPHA_EOO * rx->eoo_smooth + (1.0f - RADC_V2_ALPHA_EOO) * rx->eoo_corr;
    return rx->eoo_smooth > RADC_V2_TEOO;
}

static int update_frame_sync_and_decode(rade_rx_v2_state *rx, float *features_out,
                                        int sig_det, int sine_det) {
    float metric = rade_frame_sync_v2(&rx->sync_model, rx->z_hat_cur, /*arch=*/0);
    const float gamma = RADC_V2_TRACK_BETA;
    int winning;
    if (rx->s % 2) {
        rx->frame_sync_odd = gamma * rx->frame_sync_odd + (1.0f - gamma) * metric;
        winning = rx->frame_sync_odd > rx->frame_sync_even;
    } else {
        rx->frame_sync_even = gamma * rx->frame_sync_even + (1.0f - gamma) * metric;
        winning = rx->frame_sync_even > rx->frame_sync_odd;
    }
    if (!winning) return 0;

    memcpy(rx->az_hat, rx->z_hat_cur, sizeof(rx->az_hat));
    rx->have_az_hat = 1;
    rade_core_decoder_v2(&rx->dec_state, &rx->dec_model, features_out, rx->z_hat_cur, /*arch=*/0);

    const int F = RADC_V2_FRAMES_PER_STEP, NF = RADC_V2_NUM_FEATURES_AUX;
    if (rx->limit_pitch)
        for (int f = 0; f < F; f++)
            if (features_out[f * NF + 18] < -1.4f) features_out[f * NF + 18] = -1.4f;
    if (rx->mute && (!sig_det || sine_det))
        for (int f = 0; f < F; f++) features_out[f * NF + 0] = -5.0f;

    return F;
}

static int adjust_timing(rade_rx_v2_state *rx, int nin) {
    if (!rx->timing_adj || rx->fix_delta_hat) return nin;
    const int sym_len = rx->acq.sym_len;
    int shift = sym_len / 4;
    radc_cf *Ry = rx->acq.Ry_smooth, tmp[RADC_V2_SYM_LEN];
    if (rx->delta_hat > 3.0f * sym_len / 4.0f) {
        rx->delta_hat -= (float)shift;
        memcpy(tmp, Ry, sizeof(radc_cf) * (size_t)shift);
        memmove(Ry, &Ry[shift], sizeof(radc_cf) * (size_t)(sym_len - shift));
        memcpy(&Ry[sym_len - shift], tmp, sizeof(radc_cf) * (size_t)shift);
        nin = sym_len + shift;
    }
    if (rx->delta_hat < (float)(sym_len / 4)) {
        rx->delta_hat += (float)shift;
        memcpy(tmp, &Ry[sym_len - shift], sizeof(radc_cf) * (size_t)shift);
        memmove(&Ry[shift], Ry, sizeof(radc_cf) * (size_t)(sym_len - shift));
        memcpy(Ry, tmp, sizeof(radc_cf) * (size_t)shift);
        nin = sym_len - shift;
    }
    return nin;
}

static int process_sync(rade_rx_v2_state *rx, float *features_out, int sig_det, int sine_det) {
    const int M = rx->modem.m, Ncp = rx->modem.ncp;
    int next = RADC_V2_STATE_SYNC;

    float dphi = radc_carg(rx->acq.Ry_smooth[rx->delta_hat_g]);
    rx->freq_offset_g = -dphi * FS_F / (2.0f * (float)M_PI * (float)M);
    rx->delta_hat   = RADC_V2_TRACK_BETA * rx->delta_hat   + (1.0f - RADC_V2_TRACK_BETA) * (float)rx->delta_hat_g;
    rx->freq_offset = RADC_V2_TRACK_BETA * rx->freq_offset + (1.0f - RADC_V2_TRACK_BETA) * rx->freq_offset_g;

    if (!sig_det || sine_det) rx->count++;
    else rx->count = 0;
    if (rx->count == rx->hangover) { next = RADC_V2_STATE_IDLE; rx->count = 0; rx->count1 = 0; }

    rx->new_sig_delta_hat = fabsf((float)rx->delta_hat_g - rx->delta_hat) > (float)Ncp;
    rx->new_sig_f_hat     = fabsf(rx->freq_offset_g - rx->freq_offset) > 5.0f;
    if (sig_det && (rx->new_sig_delta_hat || rx->new_sig_f_hat)) rx->count1++;
    else rx->count1 = 0;
    if (rx->count1 == 5) { next = RADC_V2_STATE_IDLE; rx->count = 0; rx->count1 = 0; }

    extract_symbol(rx);

    if (detect_eoo(rx)) {
        rx->count = 0; rx->count1 = 0; rx->eoo_count = 0; rx->eoo_smooth = 0.0f;
        memset(rx->acq.Ry_smooth, 0, sizeof(rx->acq.Ry_smooth));
        rx->state = RADC_V2_STATE_IDLE;
        rx->eoo_detected = 1;
        return 0;
    }

    int nframes = update_frame_sync_and_decode(rx, features_out, sig_det, sine_det);
    rx->state = next;
    return nframes;
}

int rade_rx_v2_process(rade_rx_v2_state *rx, float *features_out, const radc_cf *rx_in) {
    int nin_used = rx->nin;
    rx->s += 1;
    rx->eoo_detected = 0;

    /* Optional input BPF (applied to each block; complex_bpf carries state across
       calls, equivalent to filtering the whole stream as rx2.py does). AGC and
       acquisition then see the filtered samples. */
    const radc_cf *in_s = rx_in;
    radc_cf filt[2 * RADC_V2_SYM_LEN];   /* >= max nin (sym_len + sym_len/4) */
    if (rx->bpf_en) {
        radc_bpf_process(&rx->bpf, filt, rx_in, nin_used);
        in_s = filt;
    }

    float gain = compute_gain(rx, in_s, nin_used);
    radc_acq_v2_slide(&rx->acq, in_s, nin_used, gain);
    rx->nin = rx->acq.sym_len;

    radc_acq_v2_autocorr(&rx->acq);
    int sig_det, sine_det;
    radc_acq_v2_detect(&rx->acq, &sig_det, &sine_det);
    rx->delta_hat_g = rx->acq.delta_hat_g;
    rx->snr_est_db  = rx->acq.snr_est_db;

    int nframes = 0;
    if (rx->state == RADC_V2_STATE_IDLE) {
        rx->state = process_idle(rx, sig_det, sine_det);
    } else {
        nframes = process_sync(rx, features_out, sig_det, sine_det);
        rx->nin = adjust_timing(rx, rx->nin);
    }

    if (rx->timing_adj_enable && rx->s > 0) rx->timing_adj = 1;
    return nframes;
}
