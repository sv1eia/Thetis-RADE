/*---------------------------------------------------------------------------*\

  radc_demod.c   (port of class receiver_one, radae/radae/dsp.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_demod.h"
#include <string.h>
#include <math.h>

/* DFT every symbol (CP removed at the fine time offset). nsym = Ns+2. */
static void demod_symbols(const radc_modem *m, radc_cf rx_sym[][RADC_NC],
                          const radc_cf *rx_in, int time_offset, int nsym) {
    radc_cf tbuf[RADC_M];
    for (int s = 0; s < nsym; s++) {
        radc_remove_cp(m, tbuf, &rx_in[s * (m->m + m->ncp)], time_offset);
        radc_dft(m, rx_sym[s], tbuf);
    }
}

/* 3-pilot least-squares channel estimate at every carrier from one pilot row. */
static void est_pilots(const radc_modem *m, radc_cf *est, const radc_cf *rx_pilot) {
    const int Nc = m->nc;
    const float a = m->local_path_delay_s * (float)RADC_FS;
    for (int c = 0; c < Nc; c++) {
        int cmid = c;
        if (c == 0) cmid = 1;
        if (c == Nc - 1) cmid = Nc - 2;
        radc_cf h[3];
        for (int i = 0; i < 3; i++)
            h[i] = radc_cdiv(rx_pilot[cmid - 1 + i], m->P[cmid - 1 + i]);
        radc_cf g0 = radc_czero(), g1 = radc_czero();
        for (int j = 0; j < 3; j++) {
            g0 = radc_cadd(g0, radc_cmul(m->Pmat[c][0][j], h[j]));
            g1 = radc_cadd(g1, radc_cmul(m->Pmat[c][1][j], h[j]));
        }
        est[c] = radc_cadd(g0, radc_cmul(g1, radc_cexpj(-m->w[c] * a)));
    }
}

float radc_demod_frame(const radc_modem *m, float *z_hat, const radc_cf *rx_in,
                       int time_offset, int coarse_mag) {
    const int Nc = m->nc, M = m->m, Ncp = m->ncp, Ns = m->ns;
    radc_cf rx_sym[RADC_NS + 2][RADC_NC];
    demod_symbols(m, rx_sym, rx_in, time_offset, Ns + 2);

    /* Channel estimates at the leading (symbol 0) and trailing (symbol Ns+1) pilots. */
    radc_cf est0[RADC_NC], est1[RADC_NC];
    est_pilots(m, est0, rx_sym[0]);
    est_pilots(m, est1, rx_sym[Ns + 1]);

    /* SNR estimate (update_snr_est): S1 from received start pilots, S2 from the
       imaginary residual after phase-correcting by the channel estimate. */
    float S1 = 0.0f, S2 = 0.0f;
    for (int c = 0; c < Nc; c++) {
        S1 += radc_cabs2(rx_sym[0][c]);
        float ph = radc_carg(est0[c]);
        radc_cf r = radc_cmul(rx_sym[0][c], radc_cexpj(-ph));
        S2 += r.imag * r.imag;
    }
    S2 += 1e-12f;
    float snr = S1 / (2.0f * S2) - 1.0f;
    if (snr <= 0.0f) snr = 0.1f;
    float snrdB = 10.0f * log10f(snr);
    snrdB = (snrdB - 4.1343f) / 0.7650f;            /* AWGN/MPG/MPP line fit */
    const float Rs = (float)RADC_FS / (float)M;
    float snr3k = snrdB + 10.0f * log10f(Rs * Nc / 3000.0f)
                        + 10.0f * log10f((float)(M + Ncp) / (float)M);

    /* Equalise data symbols 1..Ns: phase-correct by the interpolated channel. */
    for (int s = 1; s <= Ns; s++) {
        float t = (float)s / (float)(Ns + 1);       /* pilots at 0 and Ns+1 */
        for (int c = 0; c < Nc; c++) {
            radc_cf ch = radc_clerp(est0[c], est1[c], t);
            rx_sym[s][c] = radc_cmul(rx_sym[s][c], radc_cexpj(-radc_carg(ch)));
        }
    }

    /* Coarse magnitude normalisation from the two pilot estimates. */
    if (coarse_mag) {
        float ss = 0.0f;
        for (int c = 0; c < Nc; c++) ss += radc_cabs2(est0[c]) + radc_cabs2(est1[c]);
        float mag = sqrtf(ss / (2.0f * Nc)) + 1e-6f;
        if (m->bottleneck == 3) mag *= radc_cabs(m->P[0]) / m->pilot_gain;
        float inv = 1.0f / mag;
        for (int s = 1; s <= Ns; s++)
            for (int c = 0; c < Nc; c++)
                rx_sym[s][c] = radc_cscale(rx_sym[s][c], inv);
    }

    /* Data symbols 1..Ns -> interleaved real/imag latent floats. */
    int o = 0;
    for (int s = 1; s <= Ns; s++)
        for (int c = 0; c < Nc; c++) {
            z_hat[o++] = rx_sym[s][c].real;
            z_hat[o++] = rx_sym[s][c].imag;
        }
    return snr3k;
}

int radc_demod_eoo(const radc_modem *m, float *eoo_out, const radc_cf *rx_in,
                   int time_offset) {
    const int Nc = m->nc, Ns = m->ns;
    radc_cf rx_sym[RADC_NS + 2][RADC_NC];
    demod_symbols(m, rx_sym, rx_in, time_offset, Ns + 2);

    /* Simpler EQ: average phase from P (sym 0) and the two Pend pilots
       (sym 1 and sym Ns+1), then de-rotate every symbol. */
    for (int c = 0; c < Nc; c++) {
        radc_cf acc = radc_czero();
        acc = radc_cadd(acc, radc_cdiv(rx_sym[0][c],      m->P[c]));
        acc = radc_cadd(acc, radc_cdiv(rx_sym[1][c],      m->Pend[c]));
        acc = radc_cadd(acc, radc_cdiv(rx_sym[Ns + 1][c], m->Pend[c]));
        float ph = radc_carg(acc);
        radc_cf rot = radc_cexpj(-ph);
        for (int s = 0; s < Ns + 2; s++)
            rx_sym[s][c] = radc_cmul(rx_sym[s][c], rot);
    }

    /* EOO data lives in symbols 2..Ns (Ns-1 of them). */
    int o = 0;
    for (int s = 2; s <= Ns; s++)
        for (int c = 0; c < Nc; c++) {
            eoo_out[o++] = rx_sym[s][c].real;
            eoo_out[o++] = rx_sym[s][c].imag;
        }
    return o;
}
