/*---------------------------------------------------------------------------*\

  radc_bpf.c   (port of class complex_bpf, radae/radae/dsp.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_bpf.h"
#include <string.h>
#include <assert.h>

void radc_bpf_init(radc_bpf *f, int ntap, float fs_hz, float bandwidth_hz, float centre_hz) {
    assert(ntap <= RADC_BPF_NTAP);
    f->ntap  = ntap;
    f->alpha = 2.0f * (float)M_PI * centre_hz / fs_hz;

    /* Real low-pass sinc taps, bandwidth B (normalised). h[i] = B*sinc(n*B). */
    const float B = bandwidth_hz / fs_hz;
    for (int i = 0; i < ntap; i++) {
        float n = (float)i - (float)(ntap - 1) / 2.0f;
        f->h[i] = B * radc_sinc(n * B);
    }
    radc_bpf_reset(f);
}

void radc_bpf_reset(radc_bpf *f) {
    memset(f->mem, 0, sizeof(f->mem));
    memset(f->win, 0, sizeof(f->win));
    f->phase = radc_cone();
}

void radc_bpf_process(radc_bpf *f, radc_cf *out, const radc_cf *in, int n) {
    if (n <= 0) return;
    assert(n <= RADC_BPF_MAXLEN);
    const int hist = f->ntap - 1;

    /* Mixer step e = exp(-j*alpha); phase_vec[k] = phase * e^(k+1). */
    const radc_cf e = radc_cexpj(-f->alpha);

    /* Prepend filter history, then n baseband (mixed-down) samples. */
    memcpy(f->win, f->mem, sizeof(radc_cf) * hist);

    radc_cf pv[RADC_BPF_MAXLEN];
    radc_cf cur = f->phase;
    for (int k = 0; k < n; k++) {
        cur = radc_cmul(cur, e);
        pv[k] = cur;
        f->win[hist + k] = radc_cmul(in[k], cur);   /* mix down */
    }

    /* Convolve and mix back up: out[k] = (sum_t win[k+t]*h[t]) * conj(pv[k]). */
    for (int k = 0; k < n; k++) {
        radc_cf acc = radc_czero();
        const radc_cf *w = &f->win[k];
        for (int t = 0; t < f->ntap; t++)
            acc = radc_cadd(acc, radc_cscale(w[t], f->h[t]));
        out[k] = radc_cmul(acc, radc_cconj(pv[k]));
    }

    /* Persist state. */
    memcpy(f->mem, &f->win[n], sizeof(radc_cf) * hist);
    f->phase = pv[n - 1];
}
