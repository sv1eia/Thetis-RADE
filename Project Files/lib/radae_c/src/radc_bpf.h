/*---------------------------------------------------------------------------*\

  radc_bpf.h

  Complex band-pass filter: mix down to baseband, real symmetric sinc LPF,
  mix back up. Block-wise with persisted filter + phase state.

  Ported from class complex_bpf in radae/radae/dsp.py.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_BPF_H
#define RADC_BPF_H

#include "radc_types.h"
#include "radc_const.h"

#define RADC_BPF_MAXLEN (RADC_NMF + RADC_M)   /* max samples per process() call */

typedef struct {
    int   ntap;
    float alpha;                              /* 2*pi*centre/Fs */
    float h[RADC_BPF_NTAP];                   /* real symmetric LPF taps */
    radc_cf mem[RADC_BPF_NTAP - 1];           /* baseband filter history */
    radc_cf phase;                            /* mixer phase accumulator */
    /* scratch window: history + one block of baseband samples */
    radc_cf win[(RADC_BPF_NTAP - 1) + RADC_BPF_MAXLEN];
} radc_bpf;

/* ntap should be odd (typically 101). */
void radc_bpf_init(radc_bpf *f, int ntap, float fs_hz, float bandwidth_hz, float centre_hz);
void radc_bpf_reset(radc_bpf *f);

/* Filter n (<= RADC_BPF_MAXLEN) complex samples in -> out (may alias). */
void radc_bpf_process(radc_bpf *f, radc_cf *out, const radc_cf *in, int n);

#endif /* RADC_BPF_H */
