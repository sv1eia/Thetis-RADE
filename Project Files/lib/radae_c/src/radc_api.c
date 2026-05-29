/*---------------------------------------------------------------------------*\

  radc_api.c

  Implementation of the public RADE C API (rade_api.h) over the radc_* / rade_tx
  / rade_rx internals. Python-free. The API surface is byte-for-byte compatible
  with the reference rade_api.h so existing consumers (e.g. Thetis) link
  unchanged.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe
  BSD-2-Clause (see LICENSE).
*/

#define VERSION 2   /* Python-free API */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rade_api.h"

void rade_initialize(void) {}
void rade_finalize(void) {}
int  rade_version(void) { return VERSION; }

struct rade *rade_open(char model_file[], int flags) {
    (void)model_file;
    struct rade *r = (struct rade *)calloc(1, sizeof(struct rade));
    if (!r) return NULL;
    r->flags = flags;
    r->auxdata = 1;
    r->bottleneck = RADC_BOTTLENECK_DEFAULT;

    int bpf_tx = 0;   /* Tx BPF off by default (matches reference) */
    int bpf_rx = 1;   /* Rx BPF on by default */
    if (rade_tx_init(&r->tx, r->bottleneck, r->auxdata, bpf_tx) != 0) { free(r); return NULL; }
    if (rade_rx_init(&r->rx, r->bottleneck, r->auxdata, bpf_rx) != 0) { free(r); return NULL; }
    if (flags & RADE_VERBOSE_0) r->rx.verbose = 0;
    return r;
}

void rade_close(struct rade *r) { free(r); }

int rade_n_tx_out(struct rade *r)          { return rade_tx_n_samples_out(&r->tx); }
int rade_n_tx_eoo_out(struct rade *r)      { return rade_tx_n_eoo_out(&r->tx); }
int rade_nin_max(struct rade *r)           { return rade_rx_nin_max(&r->rx); }
int rade_n_features_in_out(struct rade *r) { return rade_tx_n_features_in(&r->tx); }
int rade_n_eoo_bits(struct rade *r)        { return rade_tx_n_eoo_bits(&r->tx); }
int rade_nin(struct rade *r)               { return rade_rx_nin(&r->rx); }

int rade_tx(struct rade *r, RADE_COMP tx_out[], float features_in[]) {
    return rade_tx_process(&r->tx, tx_out, features_in);
}
int rade_tx_eoo(struct rade *r, RADE_COMP tx_eoo_out[]) {
    return rade_tx_state_eoo(&r->tx, tx_eoo_out);
}
void rade_tx_set_eoo_bits(struct rade *r, float eoo_bits[]) {
    rade_tx_state_set_eoo_bits(&r->tx, eoo_bits);
}

void rade_tx_set_eoo_callsign(struct rade *r, const char *callsign) {
    /* Pack up to RADE_EOO_CALLSIGN_MAX chars, 7 bits MSB-first, as +/-1 QPSK. */
    int src = (int)strlen(callsign);
    for (int i = 0; i < RADE_EOO_CALLSIGN_MAX; i++) {
        unsigned char c = (i < src) ? (unsigned char)callsign[i] : ' ';
        for (int b = 0; b < 7; b++)
            r->tx.eoo_bits[i * 7 + b] = ((c >> (6 - b)) & 1) ? 1.0f : -1.0f;
    }
}

int rade_rx_get_eoo_callsign(const float *eoo_bits, int n_eoo_bits, char *callsign_out) {
    if (n_eoo_bits < RADE_EOO_CALLSIGN_MAX * 7) { callsign_out[0] = '\0'; return 0; }
    for (int i = 0; i < RADE_EOO_CALLSIGN_MAX; i++) {
        unsigned char c = 0;
        for (int b = 0; b < 7; b++)
            if (eoo_bits[i * 7 + b] > 0.0f) c |= (unsigned char)(1 << (6 - b));
        callsign_out[i] = (char)c;
    }
    callsign_out[RADE_EOO_CALLSIGN_MAX] = '\0';
    int len = RADE_EOO_CALLSIGN_MAX;
    while (len > 0 && callsign_out[len - 1] == ' ') callsign_out[--len] = '\0';
    return len;
}

int rade_rx(struct rade *r, float features_out[], int *has_eoo_out, float eoo_out[], RADE_COMP rx_in[]) {
    int ret = rade_rx_process(&r->rx, features_out, eoo_out, rx_in);
    *has_eoo_out = (ret & 0x2) ? 1 : 0;
    return (ret & 0x1) ? rade_rx_n_features_out(&r->rx) : 0;
}

int   rade_sync(struct rade *r)          { return rade_rx_sync(&r->rx); }
float rade_freq_offset(struct rade *r)   { return rade_rx_freq_offset(&r->rx); }
int   rade_snrdB_3k_est(struct rade *r)  { return (int)rade_rx_snrdB_3k_est(&r->rx); }
void  rade_set_disable_unsync(struct rade *r, float seconds) { r->rx.disable_unsync = seconds; }
