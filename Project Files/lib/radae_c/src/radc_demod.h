/*---------------------------------------------------------------------------*\

  radc_demod.h

  OFDM demodulator: remove CP, DFT, 3-pilot least-squares channel estimate,
  interpolated phase equalisation, coarse magnitude, SNR estimate, and the
  simpler EOO equalisation path.

  Ported from class receiver_one (radae/radae/dsp.py).

  The caller is expected to have already removed the carrier frequency offset
  from rx_in (radc_rx does this), exactly as radae_rxe.py does before calling
  receiver_one().

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_DEMOD_H
#define RADC_DEMOD_H

#include "radc_modem.h"

/* rx_in: (Ns+2)*(M+Ncp) samples (one frame + trailing pilot symbol). */
#define RADC_DEMOD_IN_LEN ((RADC_NS + 2) * (RADC_M + RADC_NCP))

/* Normal frame -> z_hat[Nzmf*latent_dim] floats. Returns the per-frame SNR
   estimate in a 3 kHz noise bandwidth (dB); caller smooths it. */
float radc_demod_frame(const radc_modem *m, float *z_hat, const radc_cf *rx_in,
                       int time_offset, int coarse_mag);

/* EOO frame -> eoo_out[(Ns-1)*Nc*2] floats (the EOO data symbols). */
int radc_demod_eoo(const radc_modem *m, float *eoo_out, const radc_cf *rx_in,
                   int time_offset);

#endif /* RADC_DEMOD_H */
