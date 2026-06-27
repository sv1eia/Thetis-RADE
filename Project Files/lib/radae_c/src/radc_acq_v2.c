/*---------------------------------------------------------------------------*\

  radc_acq_v2.c   (port of RADEv2Receiver._compute_autocorr / _detect_signal)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_acq_v2.h"
#include <math.h>
#include <string.h>

void radc_acq_v2_init(radc_acq_v2 *a, const radc_modem_v2 *m) {
    memset(a, 0, sizeof(*a));
    a->m = m->m; a->ncp = m->ncp; a->ns = m->ns;
    a->sym_len = m->m + m->ncp;
    a->fs = (float)RADC_V2_FS;
    a->alpha = RADC_V2_ACQ_ALPHA;
    a->tsig  = RADC_V2_TSIG;
    a->tsin  = RADC_V2_TSIN;

    /* CP correlator sees noise in B_bpf, not 3 kHz -> SNR3k offset. */
    float w0 = m->w[0], wN = m->w[m->nc - 1];
    a->b_bpf = 1.2f * (wN - w0) * a->fs / (2.0f * (float)M_PI);
    a->snr_offset_db = 10.0f * log10f(3000.0f / a->b_bpf);
    a->snr_corr_a = RADC_V2_SNR_CORR_A;
    a->snr_corr_b = RADC_V2_SNR_CORR_B;
}

void radc_acq_v2_slide(radc_acq_v2 *a, const radc_cf *rx_in, int nin, float gain) {
    int N = 3 * a->sym_len;
    memmove(a->rx_buf, &a->rx_buf[nin], sizeof(radc_cf) * (size_t)(N - nin));
    for (int i = 0; i < nin; i++)
        a->rx_buf[N - nin + i] = radc_cscale(rx_in[i], gain);
}

void radc_acq_v2_autocorr(radc_acq_v2 *a) {
    const int M = a->m, Ncp = a->ncp, sym_len = a->sym_len;

    for (int gamma = 0; gamma < sym_len; gamma++) {
        int idx = sym_len + gamma;
        /* y_cp = rx_buf[idx-Ncp : idx], y_m = rx_buf[idx-Ncp+M : idx+M] (Ncp each).
           Ry = sum y_cp * conj(y_m); D = sum|y_cp|^2 + sum|y_m|^2 + 1e-12. */
        radc_cf Ry = radc_czero();
        float Dcp = 0.0f, Dm = 0.0f;
        for (int k = 0; k < Ncp; k++) {
            radc_cf ycp = a->rx_buf[idx - Ncp + k];
            radc_cf ym  = a->rx_buf[idx - Ncp + M + k];
            Ry = radc_cadd(Ry, radc_cmul(ycp, radc_cconj(ym)));
            Dcp += radc_cabs2(ycp);
            Dm  += radc_cabs2(ym);
        }
        float D = Dcp + Dm + 1e-12f;
        a->Ry_norm[gamma] = radc_cscale(Ry, 2.0f / fabsf(D));
    }

    for (int gamma = 0; gamma < sym_len; gamma++)
        a->Ry_smooth[gamma] = radc_cadd(radc_cscale(a->Ry_smooth[gamma], a->alpha),
                                        radc_cscale(a->Ry_norm[gamma], 1.0f - a->alpha));
}

void radc_acq_v2_detect(radc_acq_v2 *a, int *sig_det, int *sine_det) {
    const int sym_len = a->sym_len;

    int amax = 0, amin = 0;
    float vmax = -1.0f, vmin = 1e30f;
    for (int g = 0; g < sym_len; g++) {
        float v = radc_cabs(a->Ry_smooth[g]);
        if (v > vmax) { vmax = v; amax = g; }   /* first max (matches np.argmax) */
        if (v < vmin) { vmin = v; amin = g; }   /* first min (matches np.argmin) */
    }
    (void)amin;
    a->delta_hat_g = amax;
    a->Ry_max = vmax;
    a->Ry_min = vmin;

    if (sig_det)  *sig_det  = (vmax > a->tsig) ? 1 : 0;
    if (sine_det) *sine_det = (vmax / (vmin + 1e-12f) < a->tsin) ? 1 : 0;

    float rho = vmax;
    if (rho < 0.0f) rho = 0.0f;
    if (rho > 1.0f - 1e-6f) rho = 1.0f - 1e-6f;
    float snr_raw = 10.0f * log10f(rho / (1.0f - rho) + 1e-12f) - a->snr_offset_db;
    a->snr_est_db = a->snr_corr_a * snr_raw + a->snr_corr_b;

    float dphi = radc_carg(a->Ry_smooth[amax]);
    a->freq_offset_inst = -dphi * a->fs / (2.0f * (float)M_PI * (float)a->m);
}
