/*---------------------------------------------------------------------------*\

  radc_modem_v2.h

  OFDM modem setup for RADE V2: carrier frequencies, IDFT/DFT matrices, the
  (alternate-sign-flipped) EOO pilot in freq + time domain with cyclic prefix,
  the EOO gain, and the pre-computed V2 End-Of-Over frame (six pend_cp symbols).

  Unlike V1 there are NO data-frame pilots and NO per-carrier least-squares EQ
  matrices (channel equalisation is learned end-to-end by the decoder). The pilot
  sequences (P/Pend/p/pend/p_cp/pend_cp) are still built because the EOO frame and
  the V2 EOO detector use pend / pend_cp.

  Ported from radae/radae/radae.py (RADAE.__init__, V2 configuration).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_MODEM_V2_H
#define RADC_MODEM_V2_H

#include "radc_types.h"
#include "radc_const_v2.h"

typedef struct {
    int nc, m, ncp, ns;

    float w[RADC_V2_NC];                            /* angular freq per carrier (rad/sample) */
    radc_cf Winv[RADC_V2_NC][RADC_V2_M];            /* IDFT: freq[Nc] -> time[M], Tx */
    radc_cf Wfwd[RADC_V2_M][RADC_V2_NC];            /* DFT:  time[M] -> freq[Nc], Rx */

    radc_cf P[RADC_V2_NC];                          /* Barker pilots (freq domain) */
    radc_cf Pend[RADC_V2_NC];                       /* EOO pilots (odd-carrier sign flip) */
    radc_cf p[RADC_V2_M];                           /* time-domain pilot (no CP) */
    radc_cf pend[RADC_V2_M];                        /* time-domain EOO pilot (no CP) */
    radc_cf p_cp[RADC_V2_SYM_LEN];                  /* time-domain pilot with CP */
    radc_cf pend_cp[RADC_V2_SYM_LEN];               /* time-domain EOO pilot with CP */

    float pilot_gain_eoo_v2;                        /* 10^(-8/20) * M / sqrt(Nc) */
    radc_cf eoo_v2[RADC_V2_NEOO];                   /* pre-computed V2 EOO frame (6 x pend_cp) */
} radc_modem_v2;

/* Build all matrices / pilots / EOO frame. */
void radc_modem_v2_init(radc_modem_v2 *m);

/* IDFT: freq_in[Nc] -> time_out[M]   (time_out[n] = sum_c freq_in[c]*Winv[c][n]) */
void radc_idft_v2(const radc_modem_v2 *m, radc_cf *time_out, const radc_cf *freq_in);

/* DFT: time_in[M] -> freq_out[Nc]    (freq_out[c] = sum_n time_in[n]*Wfwd[n][c]) */
void radc_dft_v2(const radc_modem_v2 *m, radc_cf *freq_out, const radc_cf *time_in);

/* Insert/strip cyclic prefix. time_in[M] -> cp_out[M+Ncp]; cp_in[M+Ncp]+off -> time_out[M]. */
void radc_insert_cp_v2(const radc_modem_v2 *m, radc_cf *cp_out, const radc_cf *time_in);
void radc_remove_cp_v2(const radc_modem_v2 *m, radc_cf *time_out, const radc_cf *cp_in, int time_offset);

#endif /* RADC_MODEM_V2_H */
