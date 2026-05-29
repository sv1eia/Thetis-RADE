/*---------------------------------------------------------------------------*\

  radc_acq.h

  Pilot acquisition / sync correlation: coarse time-frequency pilot detection,
  fine refinement, and per-frame pilot + EOO spot checks with an adaptive
  noise-statistics threshold.

  Ported from class acquisition (radae/radae/dsp.py).

  Re-entrancy: the noise-grid refresh uses a PER-INSTANCE xorshift PRNG (held in
  this struct), NOT the process-global rand(), so concurrent handles on separate
  threads are race-free and independently reproducible (plan B.5.1).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_ACQ_H
#define RADC_ACQ_H

#include <stdint.h>
#include "radc_modem.h"

typedef struct {
    int m, ncp, nmf;
    int nfreq;
    float fs;
    float fcoarse[RADC_ACQ_NFREQ];
    radc_cf p[RADC_M];
    radc_cf pend[RADC_M];
    radc_cf p_w[RADC_ACQ_NFREQ][RADC_M];      /* freq-shifted pilots for detect */

    /* Correlation grids (persist between detect/check for sigma_r tracking). */
    radc_cf Dt1[RADC_NMF][RADC_ACQ_NFREQ];
    radc_cf Dt2[RADC_NMF][RADC_ACQ_NFREQ];

    float Pacq_error1, Pacq_error2;

    /* Diagnostics (mirror the reference's printed fields). */
    float Dthresh, Dtmax12, Dtmax12_eoo;

    uint32_t rng_state;                       /* per-instance PRNG (B.5.1) */
} radc_acq;

void radc_acq_init(radc_acq *a, const radc_modem *m, float frange, float fstep, uint32_t seed);

/* Coarse search over rx[RADC_RX_BUF_SIZE]; sets *tmax,*fmax; returns candidate. */
int  radc_acq_detect(radc_acq *a, const radc_cf *rx, int *tmax, float *fmax);

/* Fine search around (*tmax,*fmax) over [t0,t1) x [f0,f1) step fstep. */
void radc_acq_refine(radc_acq *a, const radc_cf *rx, int *tmax, float *fmax,
                     int t0, int t1, float f0, float f1, float fstep);

/* Per-frame spot check at (tmax,fmax): sets *valid and *endofover. */
void radc_acq_check(radc_acq *a, const radc_cf *rx, int tmax, float fmax,
                    int *valid, int *endofover);

#endif /* RADC_ACQ_H */
