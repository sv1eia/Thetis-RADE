/*---------------------------------------------------------------------------*\

  radc_mod.c   (port of class transmitter_one, radae/radae/dsp.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#include "radc_mod.h"
#include <string.h>
#include <assert.h>

int radc_mod_frame(const radc_modem *m, radc_cf *tx_out, const float *z) {
    const int Nc = m->nc, M = m->m, Ncp = m->ncp, Ns = m->ns;

    /* z (Nzmf*latent_dim floats) -> Ns*Nc QPSK symbols (real/imag interleaved). */
    radc_cf sym[RADC_NS][RADC_NC];
    int k = 0;
    for (int s = 0; s < Ns; s++) {
        for (int c = 0; c < Nc; c++) {
            radc_cf q = radc_c(z[2 * k], z[2 * k + 1]);
            if (m->bottleneck == 2) q = radc_tanh_limit(q);
            sym[s][c] = q;
            k++;
        }
    }

    radc_cf pilot[RADC_NC];
    for (int c = 0; c < Nc; c++) pilot[c] = radc_cscale(m->P[c], m->pilot_gain);

    radc_cf tbuf[RADC_M];
    int out = 0;

    /* Pilot symbol, then Ns data symbols; each IDFT'd and CP-prefixed. */
    radc_idft(m, tbuf, pilot);
    radc_insert_cp(m, &tx_out[out], tbuf);
    out += M + Ncp;
    for (int s = 0; s < Ns; s++) {
        radc_idft(m, tbuf, sym[s]);
        radc_insert_cp(m, &tx_out[out], tbuf);
        out += M + Ncp;
    }

    /* bottleneck 3: PA saturation on the rate-Fs time-domain signal. */
    if (m->bottleneck == 3)
        for (int n = 0; n < out; n++) tx_out[n] = radc_tanh_limit(tx_out[n]);

    assert(out == RADC_NMF);
    return out;
}

int radc_mod_eoo(const radc_modem *m, radc_cf *tx_out, const float *eoo_bits) {
    const int Nc = m->nc, M = m->m, Ncp = m->ncp, Ns = m->ns;

    /* Start from the pre-computed P E 0 0 0 E frame. */
    memcpy(tx_out, m->eoo, sizeof(radc_cf) * RADC_NEOO);

    /* Overwrite the (Ns-1) data slots (frame positions 2..Ns) with QPSK from
       eoo_bits, mirroring the pilot-gain/PA handling of the surrounding frame. */
    if (eoo_bits) {
        radc_cf freq[RADC_NC], tbuf[RADC_M];
        for (int d = 0; d < Ns - 1; d++) {
            int pos = d + 2;
            for (int c = 0; c < Nc; c++) {
                int bi = (d * Nc + c) * 2;
                freq[c] = radc_c(eoo_bits[bi], eoo_bits[bi + 1]);
            }
            radc_idft(m, tbuf, freq);
            for (int n = 0; n < M; n++) {
                tbuf[n] = radc_cscale(tbuf[n], m->pilot_gain);
                if (m->bottleneck == 3) tbuf[n] = radc_tanh_limit(tbuf[n]);
            }
            radc_insert_cp(m, &tx_out[pos * (M + Ncp)], tbuf);
        }
    }
    return RADC_NEOO;
}
