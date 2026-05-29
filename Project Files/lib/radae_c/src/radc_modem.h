/*---------------------------------------------------------------------------*\

  radc_modem.h

  Shared OFDM modem setup for RADE V1: carrier frequencies, IDFT/DFT matrices,
  Barker pilots (time + freq domain, with cyclic prefix), pilot gain, the
  pre-computed End-Of-Over (EOO) frame, and the per-carrier 3-pilot least-squares
  equalisation matrices.

  Ported from radae/radae/radae.py (RADAE.__init__).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_MODEM_H
#define RADC_MODEM_H

#include "radc_types.h"
#include "radc_const.h"

typedef struct {
    int nc, m, ncp, ns;
    int bottleneck;

    float w[RADC_NC];                          /* angular freq per carrier (rad/sample) */
    radc_cf Winv[RADC_NC][RADC_M];             /* IDFT: freq[Nc] -> time[M], Tx (P @ Winv) */
    radc_cf Wfwd[RADC_M][RADC_NC];             /* DFT:  time[M] -> freq[Nc], Rx (rx @ Wfwd) */

    radc_cf P[RADC_NC];                        /* Barker pilots (freq domain) */
    radc_cf Pend[RADC_NC];                     /* EOO pilots (odd-carrier sign flip) */
    radc_cf p[RADC_M];                         /* time-domain pilot (no CP) */
    radc_cf pend[RADC_M];                      /* time-domain EOO pilot (no CP) */
    radc_cf p_cp[RADC_M + RADC_NCP];           /* time-domain pilot with CP */
    radc_cf pend_cp[RADC_M + RADC_NCP];        /* time-domain EOO pilot with CP */
    float pilot_gain;

    radc_cf eoo[RADC_NEOO];                    /* pre-computed EOO frame */

    float local_path_delay_s;
    radc_cf Pmat[RADC_NC][2][3];               /* per-carrier (A^H A)^-1 A^H */
} radc_modem;

/* Initialise all matrices/pilots for the given bottleneck mode. */
void radc_modem_init(radc_modem *m, int bottleneck);

/* IDFT: freq_in[Nc] -> time_out[M]   (time_out[n] = sum_c freq_in[c]*Winv[c][n]) */
void radc_idft(const radc_modem *m, radc_cf *time_out, const radc_cf *freq_in);

/* DFT: time_in[M] -> freq_out[Nc]    (freq_out[c] = sum_n time_in[n]*Wfwd[n][c]) */
void radc_dft(const radc_modem *m, radc_cf *freq_out, const radc_cf *time_in);

/* Insert/strip cyclic prefix. time_in[M] -> cp_out[M+Ncp]; cp_in[M+Ncp]+off -> time_out[M]. */
void radc_insert_cp(const radc_modem *m, radc_cf *cp_out, const radc_cf *time_in);
void radc_remove_cp(const radc_modem *m, radc_cf *time_out, const radc_cf *cp_in, int time_offset);

#endif /* RADC_MODEM_H */
