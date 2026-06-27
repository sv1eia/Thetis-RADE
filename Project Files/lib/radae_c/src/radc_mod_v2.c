/*---------------------------------------------------------------------------*\

  radc_mod_v2.c   (port of RADEv2Transmitter.transmit_frame / eoo, radae_v2.py)

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#include "radc_mod_v2.h"
#include <string.h>
#include <assert.h>

int radc_mod_v2_frame(const radc_modem_v2 *m, radc_cf *tx_out, const float *z) {
    const int Nc = m->nc, M = m->m, Ncp = m->ncp, Ns = m->ns;

    /* z (latent_dim floats) -> Ns*Nc QPSK symbols: sym = z[::2] + j*z[1::2],
       laid out (Ns, Nc) row-major (s outer, c inner), as in transmit_frame. */
    radc_cf sym[RADC_V2_NS][RADC_V2_NC];
    int k = 0;
    for (int s = 0; s < Ns; s++)
        for (int c = 0; c < Nc; c++) {
            sym[s][c] = radc_c(z[2 * k], z[2 * k + 1]);
            k++;
        }

    /* Each symbol: IDFT (Winv) then insert cyclic prefix. No pilot, no gain. */
    radc_cf tbuf[RADC_V2_M];
    int out = 0;
    for (int s = 0; s < Ns; s++) {
        radc_idft_v2(m, tbuf, sym[s]);
        radc_insert_cp_v2(m, &tx_out[out], tbuf);
        out += M + Ncp;
    }

    assert(out == RADC_V2_NMF);
    return out;
}

int radc_mod_v2_eoo(const radc_modem_v2 *m, radc_cf *tx_out) {
    memcpy(tx_out, m->eoo_v2, sizeof(radc_cf) * RADC_V2_NEOO);
    return RADC_V2_NEOO;
}
