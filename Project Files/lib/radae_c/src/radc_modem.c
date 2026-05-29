/*---------------------------------------------------------------------------*\

  radc_modem.c   (port of RADAE.__init__ OFDM setup, radae/radae/radae.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_modem.h"
#include <string.h>
#include <math.h>

/* Barker-13 sequence (good autocorrelation), as in barker_pilots(). */
static const float barker13[RADC_BARKER_LEN] = {
    1.f, 1.f, 1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f, -1.f, 1.f
};

void radc_modem_init(radc_modem *m, int bottleneck) {
    const int Nc = RADC_NC, M = RADC_M, Ncp = RADC_NCP, Ns = RADC_NS;
    const float Fs = (float)RADC_FS;

    memset(m, 0, sizeof(*m));
    m->nc = Nc; m->m = M; m->ncp = Ncp; m->ns = Ns;
    m->bottleneck = bottleneck;
    m->local_path_delay_s = RADC_LOCAL_PATH_DELAY_S;

    /* Carrier frequencies: centre the Nc carriers on 1500 Hz. */
    const float Rs_dash = Fs / (float)M;                       /* 50 Hz */
    const float carrier_1_freq = 1500.0f - Rs_dash * Nc / 2.0f; /* 750 Hz */
    const int   carrier_1_index = (int)lroundf(carrier_1_freq / Rs_dash); /* 15 */
    for (int c = 0; c < Nc; c++)
        m->w[c] = 2.0f * (float)M_PI * (float)(carrier_1_index + c) / (float)M;

    /* IDFT / DFT matrices: Winv[c][n]=exp(j w[c] n)/M ; Wfwd[n][c]=exp(-j w[c] n). */
    for (int c = 0; c < Nc; c++) {
        for (int n = 0; n < M; n++) {
            m->Winv[c][n] = radc_cscale(radc_cexpj(m->w[c] * (float)n), 1.0f / (float)M);
            m->Wfwd[n][c] = radc_cexpj(-m->w[c] * (float)n);
        }
    }

    /* Pilots: P = sqrt(2)*barker (real); Pend = P with odd-carrier sign flip. */
    const float scale = sqrtf(2.0f);
    for (int c = 0; c < Nc; c++) {
        m->P[c] = radc_c(scale * barker13[c % RADC_BARKER_LEN], 0.0f);
        m->Pend[c] = (c & 1) ? radc_cscale(m->P[c], -1.0f) : m->P[c];
    }

    /* Time-domain pilots p = IDFT(P), pend = IDFT(Pend). */
    for (int n = 0; n < M; n++) {
        radc_cf sp = radc_czero(), se = radc_czero();
        for (int c = 0; c < Nc; c++) {
            sp = radc_cadd(sp, radc_cmul(m->P[c], m->Winv[c][n]));
            se = radc_cadd(se, radc_cmul(m->Pend[c], m->Winv[c][n]));
        }
        m->p[n] = sp; m->pend[n] = se;
    }

    /* Add cyclic prefix (last Ncp samples copied to the front). */
    for (int n = 0; n < M; n++) {
        m->p_cp[Ncp + n] = m->p[n];
        m->pend_cp[Ncp + n] = m->pend[n];
    }
    for (int n = 0; n < Ncp; n++) {
        m->p_cp[n] = m->p[M - Ncp + n];
        m->pend_cp[n] = m->pend[M - Ncp + n];
    }

    /* Pilot gain. bottleneck 3 backs off -2 dB and scales for the PA model. */
    if (bottleneck == 3) {
        float backoff = powf(10.0f, -2.0f / 20.0f);
        m->pilot_gain = backoff * (float)M / sqrtf((float)Nc);
    } else {
        m->pilot_gain = 1.0f;
    }

    /* Pre-computed EOO frame:  P E 0 0 0 E  (P=p_cp, E=pend_cp, 0=zeros). */
    const int Nmf = (Ns + 1) * (M + Ncp);
    for (int n = 0; n < M + Ncp; n++) {
        m->eoo[n]                 = radc_cscale(m->p_cp[n],    m->pilot_gain);
        m->eoo[(M + Ncp) + n]     = radc_cscale(m->pend_cp[n], m->pilot_gain);
        m->eoo[Nmf + n]           = radc_cscale(m->pend_cp[n], m->pilot_gain);
    }
    if (bottleneck == 3)
        for (int n = 0; n < RADC_NEOO; n++) m->eoo[n] = radc_tanh_limit(m->eoo[n]);

    /* Per-carrier 3-pilot least-squares EQ matrices Pmat = (A^H A)^-1 A^H,
       A = [[1, exp(-j w[cmid-1] a)],[1, exp(-j w[cmid] a)],[1, exp(-j w[cmid+1] a)]],
       a = local_path_delay_s * Fs. Edge carriers clamp cmid. */
    const float a = m->local_path_delay_s * Fs;
    for (int c = 0; c < Nc; c++) {
        int cmid = c;
        if (c == 0) cmid = 1;
        if (c == Nc - 1) cmid = Nc - 2;

        radc_cf A[3][2];
        for (int i = 0; i < 3; i++) {
            A[i][0] = radc_cone();
            A[i][1] = radc_cexpj(-m->w[cmid - 1 + i] * a);
        }
        radc_cf AHA[2][2];
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++) {
                radc_cf s = radc_czero();
                for (int k = 0; k < 3; k++)
                    s = radc_cadd(s, radc_cmul(radc_cconj(A[k][i]), A[k][j]));
                AHA[i][j] = s;
            }
        radc_cf det = radc_csub(radc_cmul(AHA[0][0], AHA[1][1]),
                                radc_cmul(AHA[0][1], AHA[1][0]));
        radc_cf inv[2][2];
        inv[0][0] = radc_cdiv(AHA[1][1], det);
        inv[0][1] = radc_cdiv(radc_cscale(AHA[0][1], -1.0f), det);
        inv[1][0] = radc_cdiv(radc_cscale(AHA[1][0], -1.0f), det);
        inv[1][1] = radc_cdiv(AHA[0][0], det);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 3; j++) {
                radc_cf s = radc_czero();
                for (int k = 0; k < 2; k++)
                    s = radc_cadd(s, radc_cmul(inv[i][k], radc_cconj(A[j][k])));
                m->Pmat[c][i][j] = s;
            }
    }
}

void radc_idft(const radc_modem *m, radc_cf *time_out, const radc_cf *freq_in) {
    for (int n = 0; n < m->m; n++) {
        radc_cf s = radc_czero();
        for (int c = 0; c < m->nc; c++)
            s = radc_cadd(s, radc_cmul(freq_in[c], m->Winv[c][n]));
        time_out[n] = s;
    }
}

void radc_dft(const radc_modem *m, radc_cf *freq_out, const radc_cf *time_in) {
    for (int c = 0; c < m->nc; c++) {
        radc_cf s = radc_czero();
        for (int n = 0; n < m->m; n++)
            s = radc_cadd(s, radc_cmul(time_in[n], m->Wfwd[n][c]));
        freq_out[c] = s;
    }
}

void radc_insert_cp(const radc_modem *m, radc_cf *cp_out, const radc_cf *time_in) {
    memcpy(cp_out, &time_in[m->m - m->ncp], sizeof(radc_cf) * m->ncp);
    memcpy(&cp_out[m->ncp], time_in, sizeof(radc_cf) * m->m);
}

void radc_remove_cp(const radc_modem *m, radc_cf *time_out, const radc_cf *cp_in, int time_offset) {
    memcpy(time_out, &cp_in[m->ncp + time_offset], sizeof(radc_cf) * m->m);
}
