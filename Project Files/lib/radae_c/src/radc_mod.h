/*---------------------------------------------------------------------------*\

  radc_mod.h

  OFDM modulator: map a modem frame of latent vectors to rate-Fs IQ samples,
  and emit the End-Of-Over frame (with optional EOO data symbols).

  Ported from class transmitter_one (radae/radae/dsp.py) and the EOO frame
  assembly in RADAE.__init__.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_MOD_H
#define RADC_MOD_H

#include "radc_modem.h"

/* Modulate one modem frame: z[Nzmf*latent_dim] floats -> tx_out[Nmf] IQ.
   Returns Nmf. */
int radc_mod_frame(const radc_modem *m, radc_cf *tx_out, const float *z);

/* Emit the EOO frame into tx_out[Neoo], overwriting the data slots with QPSK
   symbols carried by eoo_bits[(Ns-1)*Nc*2] (all-zero if no data set).
   Returns Neoo. */
int radc_mod_eoo(const radc_modem *m, radc_cf *tx_out, const float *eoo_bits);

#endif /* RADC_MOD_H */
