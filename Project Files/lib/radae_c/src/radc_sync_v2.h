/*---------------------------------------------------------------------------*\

  radc_sync_v2.h

  RADE V2 ML frame-sync network (FrameSyncNet, radae/models_sync.py): a small
  stateless MLP that maps a demapped latent z[56] to the probability that the
  OFDM frame alignment is correct (two alignment possibilities -> binary
  classifier). Used by the V2 receiver to pick even/odd frame alignment.

  The forward pass rade_frame_sync_v2() is declared in rade_v2_core.h; the model
  weight struct (RADESync) + init_radesync() are in the generated rade_sync_data.h.
  There is no run-time state (no recurrence/conv history).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe (original RADE reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_SYNC_V2_H
#define RADC_SYNC_V2_H

#include "rade_v2_core.h"
#include "rade_sync_data.h"

#endif /* RADC_SYNC_V2_H */
