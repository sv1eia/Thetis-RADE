/*---------------------------------------------------------------------------*\

  radc_acq.c   (port of class acquisition, radae/radae/dsp.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_acq.h"
#include <string.h>
#include <math.h>

/* xorshift32 - per-instance PRNG replacing the reference's global rand(). */
static inline uint32_t acq_rand(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

void radc_acq_init(radc_acq *a, const radc_modem *m, float frange, float fstep, uint32_t seed) {
    memset(a, 0, sizeof(*a));
    a->m = m->m; a->ncp = m->ncp; a->nmf = RADC_NMF;
    a->fs = (float)RADC_FS;
    a->Pacq_error1 = RADC_ACQ_PERR1;
    a->Pacq_error2 = RADC_ACQ_PERR2;
    a->rng_state = seed ? seed : 0x52414445u; /* "RADE" */

    memcpy(a->p,    m->p,    sizeof(a->p));
    memcpy(a->pend, m->pend, sizeof(a->pend));

    /* Coarse frequency grid: arange(-frange/2, frange/2, fstep). */
    int nf = 0;
    for (float f = -frange / 2.0f; f < frange / 2.0f && nf < RADC_ACQ_NFREQ; f += fstep)
        a->fcoarse[nf++] = f;
    a->nfreq = nf;

    /* Pre-shift the pilot for each coarse frequency: p_w[f][n] = exp(j w n) p[n]. */
    for (int fi = 0; fi < nf; fi++) {
        float w = 2.0f * (float)M_PI * a->fcoarse[fi] / a->fs;
        for (int n = 0; n < a->m; n++)
            a->p_w[fi][n] = radc_cmul(radc_cexpj(w * (float)n), a->p[n]);
    }
}

/* sigma_r over the whole grid (Rayleigh mean / sqrt(pi/2)). */
static float grid_sigma_r(const radc_acq *a) {
    double s1 = 0.0, s2 = 0.0;
    int cnt = a->nmf * a->nfreq;
    for (int t = 0; t < a->nmf; t++)
        for (int fi = 0; fi < a->nfreq; fi++) {
            s1 += radc_cabs(a->Dt1[t][fi]);
            s2 += radc_cabs(a->Dt2[t][fi]);
        }
    float sr1 = (float)(s1 / cnt) / sqrtf((float)M_PI / 2.0f);
    float sr2 = (float)(s2 / cnt) / sqrtf((float)M_PI / 2.0f);
    return 0.5f * (sr1 + sr2);
}

int radc_acq_detect(radc_acq *a, const radc_cf *rx, int *tmax, float *fmax) {
    const int M = a->m, Nmf = a->nmf, nf = a->nfreq;
    float best = 0.0f; int t_best = 0; float f_best = 0.0f;

    for (int t = 0; t < Nmf; t++) {
        for (int fi = 0; fi < nf; fi++) {
            radc_cf d1 = radc_czero(), d2 = radc_czero();
            for (int n = 0; n < M; n++) {
                d1 = radc_cadd(d1, radc_cmul(radc_cconj(rx[t + n]),       a->p_w[fi][n]));
                d2 = radc_cadd(d2, radc_cmul(radc_cconj(rx[t + Nmf + n]), a->p_w[fi][n]));
            }
            a->Dt1[t][fi] = d1;
            a->Dt2[t][fi] = d2;
            float d12 = radc_cabs(d1) + radc_cabs(d2);
            if (d12 > best) { best = d12; t_best = t; f_best = a->fcoarse[fi]; }
        }
    }

    float sigma_r = grid_sigma_r(a);
    a->Dthresh = 2.0f * sigma_r * sqrtf(-logf(a->Pacq_error1 / 5.0f));
    a->Dtmax12 = best;
    *tmax = t_best; *fmax = f_best;
    return best > a->Dthresh;
}

void radc_acq_refine(radc_acq *a, const radc_cf *rx, int *tmax, float *fmax,
                     int t0, int t1, float f0, float f1, float fstep) {
    const int M = a->m, Nmf = a->nmf;
    float best = 0.0f; int t_best = *tmax; float f_best = *fmax;

    for (float f = f0; f < f1; f += fstep) {
        float w = 2.0f * (float)M_PI * f / a->fs;
        radc_cf wv1[RADC_M], wv2[RADC_M];
        radc_cf shift_nmf = radc_cexpj(-w * (float)Nmf);
        for (int n = 0; n < M; n++) {
            radc_cf e = radc_cexpj(-w * (float)n);
            wv1[n] = radc_cmul(e, radc_cconj(a->p[n]));
            wv2[n] = radc_cmul(radc_cmul(e, shift_nmf), radc_cconj(a->p[n]));
        }
        for (int t = t0; t < t1; t++) {
            radc_cf d1 = radc_czero(), d2 = radc_czero();
            for (int n = 0; n < M; n++) {
                d1 = radc_cadd(d1, radc_cmul(rx[t + n],       wv1[n]));
                d2 = radc_cadd(d2, radc_cmul(rx[t + Nmf + n], wv2[n]));
            }
            float d = radc_cabs(radc_cadd(d1, d2));
            if (d > best) { best = d; t_best = t; f_best = f; }
        }
    }
    *tmax = t_best; *fmax = f_best;
}

void radc_acq_check(radc_acq *a, const radc_cf *rx, int tmax, float fmax,
                    int *valid, int *endofover) {
    const int M = a->m, Ncp = a->ncp, Nmf = a->nmf, nf = a->nfreq;

    /* Refresh ~5% of the grid so sigma_r tracks the evolving channel. */
    int nupd = (int)(0.05f * Nmf);
    for (int i = 0; i < nupd; i++) {
        int t = (int)(acq_rand(&a->rng_state) % (uint32_t)Nmf);
        for (int fi = 0; fi < nf; fi++) {
            radc_cf d1 = radc_czero(), d2 = radc_czero();
            for (int n = 0; n < M; n++) {
                d1 = radc_cadd(d1, radc_cmul(radc_cconj(rx[t + n]),       a->p_w[fi][n]));
                d2 = radc_cadd(d2, radc_cmul(radc_cconj(rx[t + Nmf + n]), a->p_w[fi][n]));
            }
            a->Dt1[t][fi] = d1;
            a->Dt2[t][fi] = d2;
        }
    }

    float sigma_r = grid_sigma_r(a);
    a->Dthresh = 2.0f * sigma_r * sqrtf(-logf(a->Pacq_error2 / 5.0f));
    float dthresh_eoo = 2.0f * sigma_r * sqrtf(-logf(a->Pacq_error1 / 5.0f));

    float w = 2.0f * (float)M_PI * fmax / a->fs;

    /* Normal-pilot correlation at the current timing/freq. */
    radc_cf d1 = radc_czero(), d2 = radc_czero();
    for (int n = 0; n < M; n++) {
        radc_cf e = radc_cexpj(-w * (float)n);
        radc_cf s1 = radc_cmul(e, rx[tmax + n]);
        radc_cf s2 = radc_cmul(e, rx[tmax + Nmf + n]);
        d1 = radc_cadd(d1, radc_cmul(radc_cconj(s1), a->p[n]));
        d2 = radc_cadd(d2, radc_cmul(radc_cconj(s2), a->p[n]));
    }
    a->Dtmax12 = radc_cabs(d1) + radc_cabs(d2);

    /* EOO-pilot correlation (pend at symbol 1 and at the next-frame pilot). */
    radc_cf e1 = radc_czero(), e2 = radc_czero();
    for (int n = 0; n < M; n++) {
        radc_cf e = radc_cexpj(-w * (float)n);
        radc_cf s1 = radc_cmul(e, rx[tmax + M + Ncp + n]);
        radc_cf s2 = radc_cmul(e, rx[tmax + Nmf + n]);
        e1 = radc_cadd(e1, radc_cmul(radc_cconj(s1), a->pend[n]));
        e2 = radc_cadd(e2, radc_cmul(radc_cconj(s2), a->pend[n]));
    }
    a->Dtmax12_eoo = radc_cabs(e1) + radc_cabs(e2);

    *valid = a->Dtmax12 > a->Dthresh;
    *endofover = a->Dtmax12_eoo > dthresh_eoo;
}
