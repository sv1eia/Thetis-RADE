/*---------------------------------------------------------------------------*\

  radc_mod_v2.h

  RADE V2 OFDM modulator: maps a latent z[56] to one modem frame of IQ samples,
  and emits the V2 End-Of-Over frame.

  Ported from RADEv2Transmitter.transmit_frame / eoo (radae/radae_v2.py): QPSK
  (z[::2]+j*z[1::2]) -> reshape (Ns,Nc) -> IDFT(Winv) -> insert CP -> Ns*sym_len
  samples. V2 has NO data-frame pilot, NO pilot gain, and NO bottleneck/PA clip
  (peak=True only shapes training, not the inference waveform).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_MOD_V2_H
#define RADC_MOD_V2_H

#include "radc_modem_v2.h"

/* Modulate one V2 modem frame: z[RADC_V2_LATENT_DIM] -> RADC_V2_NMF (320) IQ
   samples. Returns the number of samples written. */
int radc_mod_v2_frame(const radc_modem_v2 *m, radc_cf *tx_out, const float *z);

/* Emit the V2 EOO frame (six pend_cp symbols, RADC_V2_NEOO = 960 samples). */
int radc_mod_v2_eoo(const radc_modem_v2 *m, radc_cf *tx_out);

#endif /* RADC_MOD_V2_H */
