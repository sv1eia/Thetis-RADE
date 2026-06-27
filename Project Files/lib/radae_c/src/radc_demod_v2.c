/*---------------------------------------------------------------------------*\

  radc_demod_v2.c  (port of RADAE.receiver pilots=False + RADEv2Receiver._detect_eoo)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_demod_v2.h"
#include <math.h>

void radc_demod_v2_frame(const radc_modem_v2 *m, float *z_hat, const radc_cf *rx_i,
                         int time_offset, int correct_time_offset) {
    const int M = m->m, Ncp = m->ncp, Ns = m->ns, Nc = m->nc, sym_len = m->m + m->ncp;

    /* Per symbol: remove CP at (Ncp+time_offset), DFT (Wfwd) -> Nc carriers. */
    radc_cf rx_sym[RADC_V2_NS][RADC_V2_NC];
    for (int s = 0; s < Ns; s++) {
        const radc_cf *rx_dash = &rx_i[s * sym_len + Ncp + time_offset];
        for (int c = 0; c < Nc; c++) {
            radc_cf acc = radc_czero();
            for (int n = 0; n < M; n++)
                acc = radc_cadd(acc, radc_cmul(rx_dash[n], m->Wfwd[n][c]));
            rx_sym[s][c] = acc;
        }
    }

    /* correct_time_offset: per-carrier phase exp(j * (-correct_time_offset) * w[c]). */
    for (int c = 0; c < Nc; c++) {
        radc_cf ph = radc_cexpj((float)(-correct_time_offset) * m->w[c]);
        for (int s = 0; s < Ns; s++)
            rx_sym[s][c] = radc_cmul(rx_sym[s][c], ph);
    }

    /* QPSK demap: flatten (Ns,Nc) row-major -> z_hat[2k]=real, z_hat[2k+1]=imag. */
    int k = 0;
    for (int s = 0; s < Ns; s++)
        for (int c = 0; c < Nc; c++) {
            z_hat[2 * k]     = rx_sym[s][c].real;
            z_hat[2 * k + 1] = rx_sym[s][c].imag;
            k++;
        }
}

float radc_demod_v2_eoo_corr(const radc_modem_v2 *m, const radc_cf *rx_sym_td) {
    const int M = m->m, Ncp = m->ncp;

    /* M-point DFTs (numpy fft convention, double precision to match np.fft).
       pend_fd = FFT(pend), rx_fd = FFT(rx_sym_td). */
    double pfd_re[RADC_V2_M], pfd_im[RADC_V2_M];
    double rfd_re[RADC_V2_M], rfd_im[RADC_V2_M];
    double maxmag = 0.0;
    for (int kk = 0; kk < M; kk++) {
        double pr = 0, pi = 0, rr = 0, ri = 0;
        for (int n = 0; n < M; n++) {
            double ang = -2.0 * M_PI * (double)kk * (double)n / (double)M;
            double c = cos(ang), s = sin(ang);
            double pn_re = m->pend[n].real, pn_im = m->pend[n].imag;
            double rn_re = rx_sym_td[n].real, rn_im = rx_sym_td[n].imag;
            pr += pn_re * c - pn_im * s;  pi += pn_re * s + pn_im * c;
            rr += rn_re * c - rn_im * s;  ri += rn_re * s + rn_im * c;
        }
        pfd_re[kk] = pr; pfd_im[kk] = pi;
        rfd_re[kk] = rr; rfd_im[kk] = ri;
        double mag = sqrt(pr * pr + pi * pi);
        if (mag > maxmag) maxmag = mag;
    }

    /* H_est = rx_fd/pend_fd on active bins, else 0. The reference stores H_est as
       complex64, so cast to float before the IFFT. */
    double Hre[RADC_V2_M], Him[RADC_V2_M];
    double thresh = maxmag * 1e-3;
    for (int kk = 0; kk < M; kk++) {
        double pm = sqrt(pfd_re[kk] * pfd_re[kk] + pfd_im[kk] * pfd_im[kk]);
        if (pm > thresh) {
            double d = pfd_re[kk] * pfd_re[kk] + pfd_im[kk] * pfd_im[kk];
            Hre[kk] = (float)((rfd_re[kk] * pfd_re[kk] + rfd_im[kk] * pfd_im[kk]) / d);
            Him[kk] = (float)((rfd_im[kk] * pfd_re[kk] - rfd_re[kk] * pfd_im[kk]) / d);
        } else {
            Hre[kk] = 0.0; Him[kk] = 0.0;
        }
    }

    /* h_est = IFFT(H_est); e_cp = energy in [0,Ncp) U [M-Ncp,M); e_total = all. */
    double e_total = 0.0, e_cp = 0.0;
    for (int n = 0; n < M; n++) {
        double hr = 0, hi = 0;
        for (int kk = 0; kk < M; kk++) {
            double ang = 2.0 * M_PI * (double)kk * (double)n / (double)M;
            double c = cos(ang), s = sin(ang);
            hr += Hre[kk] * c - Him[kk] * s;
            hi += Hre[kk] * s + Him[kk] * c;
        }
        hr /= (double)M; hi /= (double)M;
        double e = hr * hr + hi * hi;
        e_total += e;
        if (n < Ncp || n >= M - Ncp) e_cp += e;
    }
    e_total += 1e-12;
    return (float)(e_cp / e_total);
}
