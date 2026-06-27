/*---------------------------------------------------------------------------*\

  radc_internal.h

  Private definition of the opaque public handle `struct rade`. Kept out of the
  public rade_api.h so that the RADE V1 and V2 protocols can carry different
  internal state (different OFDM modem, encoder/decoder, and — for V2 — the ML
  frame-sync network) behind one handle type, user-selectable at rade_open().

  The `protocol` field (1 = V1, 2 = V2) is set from the RADE_PROTOCOL_V2 open
  flag; each rade_api entry point dispatches on it. The tx/rx state is referenced
  through void* so a handle can hold either the V1 state (rade_tx_state /
  rade_rx_state) or the V2 state (rade_tx_state_v2 / rade_rx_state_v2).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE V1 reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#ifndef RADC_INTERNAL_H
#define RADC_INTERNAL_H

#include "rade_tx.h"
#include "rade_rx.h"
#include "radc_tx_v2.h"
#include "radc_rx_v2.h"

struct rade {
    int protocol;            /* 1 = V1, 2 = V2 */
    int flags;
    int auxdata;
    int bottleneck;

    void *tx;                /* rade_tx_state* (V1) | rade_tx_state_v2* (V2) */
    void *rx;                /* rade_rx_state* (V1) | rade_rx_state_v2* (V2) */
};

#endif /* RADC_INTERNAL_H */
