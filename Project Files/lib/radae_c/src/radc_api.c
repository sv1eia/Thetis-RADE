/*---------------------------------------------------------------------------*\

  radc_api.c

  Implementation of the public RADE C API (rade_api.h) over the radc_* internals.
  Python-free. The API surface stays compatible with the reference rade_api.h so
  existing consumers (e.g. Thetis) link unchanged.

  Protocol selection (user-selectable, explicit): rade_open() inspects the
  RADE_PROTOCOL_V2 flag bit. Absent => RADE V1 (default). The opaque struct rade
  carries the selected protocol and the matching per-protocol state behind void*
  tx/rx pointers; each entry point dispatches on r->protocol. Both protocols share
  the same calling convention; the helper functions return per-protocol sizes.

  V2 differences surfaced through the API: features/IQ/nin/EOO sizes differ; the
  V2 EOO carries no data bits (rade_n_eoo_bits()==0, the EOO-bits/callsign setters
  are no-ops) - text would travel on the per-frame auxdata channel (future work).

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe (original RADE V1 reference implementation)
  Copyright (C) 2026 Christos Nikolaou (SV1EIA) <sv1eia@gmail.com> - C port
  BSD-2-Clause (see LICENSE).
*/

#define VERSION 2   /* Python-free API */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rade_api.h"
#include "radc_internal.h"

static inline rade_tx_state    *v1tx(struct rade *r) { return (rade_tx_state *)r->tx; }
static inline rade_rx_state    *v1rx(struct rade *r) { return (rade_rx_state *)r->rx; }
static inline rade_tx_v2_state *v2tx(struct rade *r) { return (rade_tx_v2_state *)r->tx; }
static inline rade_rx_v2_state *v2rx(struct rade *r) { return (rade_rx_v2_state *)r->rx; }

void rade_initialize(void) {}
void rade_finalize(void) {}
int  rade_version(void)  { return VERSION; }
int  rade_protocol(struct rade *r) { return r ? r->protocol : 0; }

struct rade *rade_open(char model_file[], int flags) {
    (void)model_file;
    struct rade *r = (struct rade *)calloc(1, sizeof(struct rade));
    if (!r) return NULL;
    r->flags = flags;
    r->auxdata = 1;
    r->bottleneck = RADC_BOTTLENECK_DEFAULT;
    r->protocol = (flags & RADE_PROTOCOL_V2) ? 2 : 1;

    if (r->protocol == 2) {
        rade_tx_v2_state *tx = (rade_tx_v2_state *)calloc(1, sizeof(rade_tx_v2_state));
        rade_rx_v2_state *rx = (rade_rx_v2_state *)calloc(1, sizeof(rade_rx_v2_state));
        if (!tx || !rx) { free(tx); free(rx); free(r); return NULL; }
        r->tx = tx; r->rx = rx;
        int agc = 1;        /* AGC enabled (RMS target = 10^(-3/20)); reference default is off */
        int bpf_rx = 1;     /* Rx input BPF on by default (matches rx2.py) */
        if (rade_tx_v2_init(tx, r->auxdata) != 0)  { rade_close(r); return NULL; }
        if (rade_rx_v2_init(rx, agc, bpf_rx) != 0) { rade_close(r); return NULL; }
        if (flags & RADE_VERBOSE_0) rx->verbose = 0;
        return r;
    }

    /* ---- RADE V1 ---- */
    int bpf_tx = 0;   /* Tx BPF off by default (matches reference) */
    int bpf_rx = 1;   /* Rx BPF on by default */
    rade_tx_state *tx = (rade_tx_state *)calloc(1, sizeof(rade_tx_state));
    rade_rx_state *rx = (rade_rx_state *)calloc(1, sizeof(rade_rx_state));
    if (!tx || !rx) { free(tx); free(rx); free(r); return NULL; }
    r->tx = tx; r->rx = rx;
    if (rade_tx_init(tx, r->bottleneck, r->auxdata, bpf_tx) != 0) { rade_close(r); return NULL; }
    if (rade_rx_init(rx, r->bottleneck, r->auxdata, bpf_rx) != 0) { rade_close(r); return NULL; }
    if (flags & RADE_VERBOSE_0) rx->verbose = 0;
    return r;
}

void rade_close(struct rade *r) {
    if (!r) return;
    free(r->tx);
    free(r->rx);
    free(r);
}

/* ---- sizing helpers (dispatch on protocol) ---- */
int rade_n_tx_out(struct rade *r) {
    return (r->protocol == 2) ? rade_tx_v2_n_samples_out(v2tx(r)) : rade_tx_n_samples_out(v1tx(r));
}
int rade_n_tx_eoo_out(struct rade *r) {
    return (r->protocol == 2) ? rade_tx_v2_n_eoo_out(v2tx(r)) : rade_tx_n_eoo_out(v1tx(r));
}
int rade_nin_max(struct rade *r) {
    return (r->protocol == 2) ? rade_rx_v2_nin_max(v2rx(r)) : rade_rx_nin_max(v1rx(r));
}
int rade_n_features_in_out(struct rade *r) {
    return (r->protocol == 2) ? rade_tx_v2_n_features_in(v2tx(r)) : rade_tx_n_features_in(v1tx(r));
}
int rade_n_eoo_bits(struct rade *r) {
    return (r->protocol == 2) ? 0 : rade_tx_n_eoo_bits(v1tx(r));   /* V2 EOO carries no bits */
}
int rade_nin(struct rade *r) {
    return (r->protocol == 2) ? rade_rx_v2_nin(v2rx(r)) : rade_rx_nin(v1rx(r));
}

/* ---- TX ---- */
int rade_tx(struct rade *r, RADE_COMP tx_out[], float features_in[]) {
    return (r->protocol == 2) ? rade_tx_v2_process(v2tx(r), tx_out, features_in)
                              : rade_tx_process(v1tx(r), tx_out, features_in);
}
int rade_tx_eoo(struct rade *r, RADE_COMP tx_eoo_out[]) {
    return (r->protocol == 2) ? rade_tx_v2_eoo(v2tx(r), tx_eoo_out)
                              : rade_tx_state_eoo(v1tx(r), tx_eoo_out);
}
void rade_tx_set_eoo_bits(struct rade *r, float eoo_bits[]) {
    if (r->protocol == 2) { (void)eoo_bits; return; }   /* V2: no EOO data bits */
    rade_tx_state_set_eoo_bits(v1tx(r), eoo_bits);
}

void rade_tx_set_eoo_callsign(struct rade *r, const char *callsign) {
    if (r->protocol == 2) { (void)callsign; return; }   /* V2: no EOO data bits */
    int src = (int)strlen(callsign);
    rade_tx_state *tx = v1tx(r);
    for (int i = 0; i < RADE_EOO_CALLSIGN_MAX; i++) {
        unsigned char c = (i < src) ? (unsigned char)callsign[i] : ' ';
        for (int b = 0; b < 7; b++)
            tx->eoo_bits[i * 7 + b] = ((c >> (6 - b)) & 1) ? 1.0f : -1.0f;
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

/* ---- RX (pull model) ---- */
int rade_rx(struct rade *r, float features_out[], int *has_eoo_out, float eoo_out[], RADE_COMP rx_in[]) {
    if (r->protocol == 2) {
        rade_rx_v2_state *rx = v2rx(r);
        float tmp[RADC_V2_FRAMES_PER_STEP * RADC_V2_NUM_FEATURES_AUX];   /* 4*21 */
        int nframes = rade_rx_v2_process(rx, tmp, (const radc_cf *)rx_in);
        *has_eoo_out = rx->eoo_detected ? 1 : 0;
        (void)eoo_out;                                                   /* V2 EOO: no data bits */
        if (nframes <= 0) return 0;
        /* pad each decoded frame (21 -> 36) into features_out, as rx2.py does */
        const int NF = RADC_V2_NUM_FEATURES_AUX, NB = RADC_V2_NB_TOTAL_FEATURES;
        for (int f = 0; f < nframes; f++) {
            for (int j = 0; j < NF; j++)  features_out[f * NB + j] = tmp[f * NF + j];
            for (int j = NF; j < NB; j++) features_out[f * NB + j] = 0.0f;
        }
        return nframes * NB;
    }
    rade_rx_state *rx = v1rx(r);
    int ret = rade_rx_process(rx, features_out, eoo_out, rx_in);
    *has_eoo_out = (ret & 0x2) ? 1 : 0;
    return (ret & 0x1) ? rade_rx_n_features_out(rx) : 0;
}

/* ---- status ---- */
int rade_sync(struct rade *r) {
    return (r->protocol == 2) ? rade_rx_v2_sync(v2rx(r)) : rade_rx_sync(v1rx(r));
}
float rade_freq_offset(struct rade *r) {
    return (r->protocol == 2) ? rade_rx_v2_freq_offset(v2rx(r)) : rade_rx_freq_offset(v1rx(r));
}
int rade_snrdB_3k_est(struct rade *r) {
    return (r->protocol == 2) ? (int)rade_rx_v2_snrdB(v2rx(r)) : (int)rade_rx_snrdB_3k_est(v1rx(r));
}
void rade_set_disable_unsync(struct rade *r, float seconds) {
    if (r->protocol == 2) { (void)seconds; return; }   /* V2 receiver has no unsync timer */
    v1rx(r)->disable_unsync = seconds;
}
