/*---------------------------------------------------------------------------*\

  radc_demod_v2.h

  RADE V2 OFDM demodulator and EOO detector.

  Demod (port of RADAE.receiver with pilots=False, radae/radae/radae.py): for each
  of Ns symbols, remove the cyclic prefix at (Ncp+time_offset), DFT (Wfwd) to Nc
  carriers, apply the correct_time_offset per-carrier phase, then QPSK-demap to the
  latent z_hat[56]. There is NO pilot channel estimation/EQ - equalisation is
  learned end-to-end by the decoder.

  EOO detector (port of RADEv2Receiver._detect_eoo): channel time-domain sparsity.
  Estimate H = FFT(rx_sym)/FFT(pend) on active bins, h = IFFT(H), and return the
  ratio of cyclic-prefix-region tap energy to total tap energy. The M-point FFTs
  are evaluated in double precision to match numpy's float64 np.fft.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_DEMOD_V2_H
#define RADC_DEMOD_V2_H

#include "radc_modem_v2.h"

/* Demodulate one V2 modem frame: rx_i[Ns*sym_len = 320] (freq-corrected, 2
   symbols with CP) -> z_hat[latent_dim = 56]. */
void radc_demod_v2_frame(const radc_modem_v2 *m, float *z_hat, const radc_cf *rx_i,
                         int time_offset, int correct_time_offset);

/* EOO channel-sparsity metric for one M-sample (post-CP) time-domain symbol
   rx_sym_td[M]: returns the instantaneous e_cp/e_total correlation (the receiver
   applies the ALPHA_EOO IIR + TEOO threshold). */
float radc_demod_v2_eoo_corr(const radc_modem_v2 *m, const radc_cf *rx_sym_td);

#endif /* RADC_DEMOD_V2_H */
