/*---------------------------------------------------------------------------*\

  radc_types.h

  Complex scalar type and inline complex arithmetic for radae_c.

  Ported from the numpy/torch complex math used throughout the RADE V1
  reference (radae/radae/dsp.py, radae/radae/radae.py).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_TYPES_H
#define RADC_TYPES_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Interleaved complex float, ABI-compatible with the public RADE_COMP and with
   numpy complex64 / interleaved I,Q float32 streams. */
typedef struct {
    float real;
    float imag;
} radc_cf;

/* Public complex type, identical layout. Same include guard as rade_api.h so
   the two definitions coexist regardless of include order. */
#ifndef __RADE_COMP__
#define __RADE_COMP__
typedef struct {
    float real;
    float imag;
} RADE_COMP;
#endif

static inline radc_cf radc_c(float re, float im) { radc_cf c = {re, im}; return c; }
static inline radc_cf radc_czero(void)           { radc_cf c = {0.0f, 0.0f}; return c; }
static inline radc_cf radc_cone(void)            { radc_cf c = {1.0f, 0.0f}; return c; }

static inline radc_cf radc_cadd(radc_cf a, radc_cf b) { return radc_c(a.real + b.real, a.imag + b.imag); }
static inline radc_cf radc_csub(radc_cf a, radc_cf b) { return radc_c(a.real - b.real, a.imag - b.imag); }

static inline radc_cf radc_cmul(radc_cf a, radc_cf b) {
    return radc_c(a.real * b.real - a.imag * b.imag,
                  a.real * b.imag + a.imag * b.real);
}

static inline radc_cf radc_cscale(radc_cf a, float s) { return radc_c(a.real * s, a.imag * s); }
static inline radc_cf radc_cconj(radc_cf a)           { return radc_c(a.real, -a.imag); }

static inline float radc_cabs2(radc_cf a) { return a.real * a.real + a.imag * a.imag; }
static inline float radc_cabs(radc_cf a)  { return sqrtf(radc_cabs2(a)); }
static inline float radc_carg(radc_cf a)  { return atan2f(a.imag, a.real); }

/* exp(j*theta) */
static inline radc_cf radc_cexpj(float theta) { return radc_c(cosf(theta), sinf(theta)); }
/* r * exp(j*theta) */
static inline radc_cf radc_cpolar(float r, float theta) { return radc_c(r * cosf(theta), r * sinf(theta)); }

static inline radc_cf radc_cdiv(radc_cf a, radc_cf b) {
    float d = b.real * b.real + b.imag * b.imag;
    return radc_c((a.real * b.real + a.imag * b.imag) / d,
                  (a.imag * b.real - a.real * b.imag) / d);
}

/* PA saturation model used by bottleneck==3: tanh(|z|) * exp(j*angle(z)).
   Matches torch.tanh(torch.abs(tx))*torch.exp(1j*torch.angle(tx)). */
static inline radc_cf radc_tanh_limit(radc_cf z) {
    float mag = radc_cabs(z);
    if (mag < 1e-20f) return radc_czero();
    float lim = tanhf(mag);
    return radc_cscale(z, lim / mag);
}

/* Complex linear interpolation a + t*(b-a). */
static inline radc_cf radc_clerp(radc_cf a, radc_cf b, float t) {
    return radc_c(a.real + t * (b.real - a.real),
                  a.imag + t * (b.imag - a.imag));
}

/* sinc(x) = sin(pi x)/(pi x), with sinc(0)=1, matching numpy.sinc. */
static inline float radc_sinc(float x) {
    if (fabsf(x) < 1e-10f) return 1.0f;
    float px = (float)M_PI * x;
    return sinf(px) / px;
}

#endif /* RADC_TYPES_H */
