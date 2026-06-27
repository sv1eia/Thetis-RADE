/*---------------------------------------------------------------------------*\

  radc_modem_v2.c   (port of RADAE.__init__ OFDM setup, V2 config, radae.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_modem_v2.h"
#include <string.h>
#include <math.h>

/* Barker-13 sequence, cyclically tiled across carriers (barker_pilots()). */
static const double barker13[RADC_V2_BARKER_LEN] = {
    1.0, 1.0, 1.0, 1.0, 1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0, 1.0
};

void radc_modem_v2_init(radc_modem_v2 *m) {
    const int Nc = RADC_V2_NC, M = RADC_V2_M, Ncp = RADC_V2_NCP, Ns = RADC_V2_NS;

    memset(m, 0, sizeof(*m));
    m->nc = Nc; m->m = M; m->ncp = Ncp; m->ns = Ns;

    /* Carrier angular frequencies w[c] = 2*pi*(carrier_1_index + c)/M.
       Reproduce the reference's float32 arithmetic sequence exactly: torch
       evaluates 2*pi * arange / M in the default float32 dtype, so each w[c] is
       a float32 with intermediate rounding. Computing in double then rounding
       differs by ~1 ulp, which the angle n*w[c] (n up to M-1) amplifies to
       ~1.5e-5 and breaks waveform parity. */
    const float two_pi = (float)(2.0 * M_PI);
    for (int c = 0; c < Nc; c++)
        m->w[c] = two_pi * (float)(RADC_V2_CARRIER1_INDEX + c) / (float)M;

    /* IDFT / DFT matrices: Winv[c][n]=exp(j w[c] n)/M ; Wfwd[n][c]=exp(-j w[c] n).
       Computed in float32 to match the reference: torch promotes
       (python float * int64 arange) to the default float32 dtype, so self.w and
       torch.exp(1j*arange(M)*w[c]) are evaluated in float32/complex64. Using
       double here would be *more* accurate than Python and break waveform parity
       (~1.8e-5 mismatch at the larger angles). */
    for (int c = 0; c < Nc; c++) {
        float wc = m->w[c];
        for (int n = 0; n < M; n++) {
            float ang = wc * (float)n;
            m->Winv[c][n] = radc_c(cosf(ang) / (float)M, sinf(ang) / (float)M);
            m->Wfwd[n][c] = radc_c(cosf(-ang), sinf(-ang));
        }
    }

    /* Pilots: P = sqrt(2)*barker (real); Pend = P with odd-carrier sign flip. */
    const double scale = sqrt(2.0);
    for (int c = 0; c < Nc; c++) {
        float pv = (float)(scale * barker13[c % RADC_V2_BARKER_LEN]);
        m->P[c]    = radc_c(pv, 0.0f);
        m->Pend[c] = (c & 1) ? radc_c(-pv, 0.0f) : radc_c(pv, 0.0f);
    }

    /* Time-domain pilots p = IDFT(P), pend = IDFT(Pend). float32 matmul to match
       the reference complex64 torch.matmul(P, Winv). */
    for (int n = 0; n < M; n++) {
        radc_cf sp = radc_czero(), se = radc_czero();
        for (int c = 0; c < Nc; c++) {
            sp = radc_cadd(sp, radc_cmul(m->P[c],    m->Winv[c][n]));
            se = radc_cadd(se, radc_cmul(m->Pend[c], m->Winv[c][n]));
        }
        m->p[n] = sp; m->pend[n] = se;
    }

    /* Add cyclic prefix (last Ncp samples copied to the front). */
    for (int n = 0; n < M; n++) {
        m->p_cp[Ncp + n]    = m->p[n];
        m->pend_cp[Ncp + n] = m->pend[n];
    }
    for (int n = 0; n < Ncp; n++) {
        m->p_cp[n]    = m->p[M - Ncp + n];
        m->pend_cp[n] = m->pend[M - Ncp + n];
    }

    /* V2 EOO: gain backoff -8 dB (so peak <= 1), frame = six pend_cp symbols.
       gain computed in double (reference: 10^(-8/20) * M / Nc**0.5). */
    m->pilot_gain_eoo_v2 =
        (float)(pow(10.0, RADC_V2_EOO_BACKOFF_DB / 20.0) * (double)M / sqrt((double)Nc));
    for (int i = 0; i < RADC_V2_N_EOO; i++)
        for (int n = 0; n < M + Ncp; n++)
            m->eoo_v2[i * (M + Ncp) + n] =
                radc_cscale(m->pend_cp[n], m->pilot_gain_eoo_v2);
}

void radc_idft_v2(const radc_modem_v2 *m, radc_cf *time_out, const radc_cf *freq_in) {
    for (int n = 0; n < m->m; n++) {
        radc_cf s = radc_czero();
        for (int c = 0; c < m->nc; c++)
            s = radc_cadd(s, radc_cmul(freq_in[c], m->Winv[c][n]));
        time_out[n] = s;
    }
}

void radc_dft_v2(const radc_modem_v2 *m, radc_cf *freq_out, const radc_cf *time_in) {
    for (int c = 0; c < m->nc; c++) {
        radc_cf s = radc_czero();
        for (int n = 0; n < m->m; n++)
            s = radc_cadd(s, radc_cmul(time_in[n], m->Wfwd[n][c]));
        freq_out[c] = s;
    }
}

void radc_insert_cp_v2(const radc_modem_v2 *m, radc_cf *cp_out, const radc_cf *time_in) {
    memcpy(cp_out, &time_in[m->m - m->ncp], sizeof(radc_cf) * m->ncp);
    memcpy(&cp_out[m->ncp], time_in, sizeof(radc_cf) * m->m);
}

void radc_remove_cp_v2(const radc_modem_v2 *m, radc_cf *time_out, const radc_cf *cp_in, int time_offset) {
    memcpy(time_out, &cp_in[m->ncp + time_offset], sizeof(radc_cf) * m->m);
}
