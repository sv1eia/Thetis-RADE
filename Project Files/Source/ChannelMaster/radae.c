/*  radae.c
 *
 *  RADE V1 (Radio AutoEncoder) digital-voice integration into Thetis.
 *
 *  Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
 *  radae_nopy + Opus DNN + r8brain + FreeDV-GUI rade_text upstreams keep
 *  their own licences (BSD-2-Clause / MIT / LGPL-2.1) -- see the
 *  commit_pin.txt file in each lib/<vendor> directory.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA  02110-1301  USA
 */
/*
----------------------------------------------------------------------------------------------
Modified by Christos Nikolaou (SV1EIA) 2026 -- thetis-rade fork.
Christos Nikolaou can be reached by email at : sv1eia@gmail.com
----------------------------------------------------------------------------------------------
*/

#include "cmcomm.h"
#include "radae.h"

#include "rade_api.h"

#include "lpcnet.h"
#include "fargan.h"
#include "cpu_support.h"

#include "r8brain_wrap.h"

/* FreeDV-GUI's reliable-text codec — vendored under lib/freedv_text/. */
#include "rade_text.h"

#include "radae_micdsp.h"

#include <math.h>

/* WDSP mono-float resampler -- still used on RX */
extern void* create_resampleFV(int in_rate, int out_rate);
extern void  xresampleFV(float* input, float* output, int numsamps,
                         int* outsamps, void* ptr);
extern void  destroy_resampleFV(void* ptr);

/* WDSP rmatchV (PORT-exported by wdsp.dll). */
extern __declspec(dllimport) void* create_rmatchV(int in_size, int out_size,
                                                  int nom_inrate, int nom_outrate,
                                                  int ringsize, double var);
extern __declspec(dllimport) void  destroy_rmatchV(void* p);
extern __declspec(dllimport) void  xrmatchIN(void* b, double* in);
extern __declspec(dllimport) void  xrmatchOUT(void* b, double* out);
extern __declspec(dllimport) void  getRMatchDiags(void* b, int* underflows, int* overflows,
                                                  double* var, int* ringsize, int* nring);
extern __declspec(dllimport) void  resetRMatchDiags(void* b);

extern __declspec(dllimport) void  FlushDexpAudioDelay(int id);

/* ============================================================
 * Dual-RX support.  RADE decode runs concurrently on RX1 and RX2;
 * the TX side remains single (one TXA chain, one encoder).
 * ============================================================ */
#define RADAE_NRX 2

/* ============================================================
 * State
 * ============================================================ */

/* TX-side master enable + per-over latches (single). */
static volatile long g_radae_tx_enabled       = 0;
static volatile long g_radae_eoo_pending      = 0;
static volatile long g_radae_box_pending      = 0;   /* RX->MOX edge flag */

/* Per-RX RX-side master enable. */
static volatile long g_radae_rx_enabled[RADAE_NRX]       = { 0 };

/* Per-RX loopback enable.  When set, the TX-side modem audio is pushed
 * into that RX's loopback bridge in addition to (in place of) mic_io. */
static volatile long g_radae_loopback_enabled[RADAE_NRX] = { 0 };

/* CPU-utilisation gate.  Global -- only one half of the pipeline runs
 * at a time outside of loopback. */
static volatile long g_radae_mox_state        = 0;
static volatile long g_radae_drain_blocks     = 0;

/* EOO-safe un-key handshake (PTTRADE arbiter, Option B). */
static volatile long g_radae_eoo_inflight     = 0; /* EOO pushed, not yet drained out of mic_io */
static volatile long g_radae_eoo_flushed      = 0; /* EOO + 60ms silence have left mic_io        */
static volatile long g_radae_tx_silence_hold  = 0; /* keep gate passing (silence) while keyed     */
static volatile long g_radae_eoo_repeats_left = 0; /* scheduled EOO frames still to emit (multi-EOO tail) */

/* Per-RX sync / SNR registers, published from the audio thread. */
static volatile long g_radae_sync[RADAE_NRX]    = { 0 };
static volatile long g_radae_snr_db[RADAE_NRX]  = { 0 };

/* Per-RX dial frequency offset (Hz). */
static          float g_radae_freq_off[RADAE_NRX] = { 0.0f };

/* Per-RX linear gain factors.  See the comment block in the original
 * single-RX implementation -- semantics unchanged, just per-receiver. */
static volatile float g_radae_mic_scale          = 1.0f;
static volatile float g_radae_rx_scale[RADAE_NRX]      = { 1.0f, 1.0f };
static volatile float g_radae_rx_dial_scale[RADAE_NRX] = { 1.0f, 1.0f };
static volatile float g_radae_rx_af_gain[RADAE_NRX]    = { 1.0f, 1.0f };

/* Per-RX per-second peak and dBFS publication.  Driven by the
 * decoder-input level computation in xradae_rx. */
static float         g_rx_in_peak[RADAE_NRX]              = { 0.0f };
static volatile long g_rx_in_level_dbfs_q[RADAE_NRX]      = { -120, -120 };
static volatile long g_rx_in_clip_last_tick[RADAE_NRX]    = { 0, 0 };
#define RADAE_CLIP_HOLD_MS 500

/* Per-RX most-recent decoded remote callsign.  Written by the
 * rade_text codec callback (rx-indexed via the user-state arg). */
#define RADAE_REMOTE_CALL_CAP 16
static char  g_rx_remote_callsign[RADAE_NRX][RADAE_REMOTE_CALL_CAP] = { { 0 } };
static volatile long g_rx_remote_callsign_seq[RADAE_NRX] = { 0 };
static volatile long g_eoo_decode_last_tick[RADAE_NRX]   = { 0 };
#define RADAE_EOO_PULSE_MS 500

/* FreeDV-GUI rade_text codec.  TX uses a single instance to encode the
 * outgoing EOO callsign; RX uses a SEPARATE instance per receiver so
 * RX1's and RX2's decoded callsigns do not collide.  The callback's
 * user-state slot carries the rx index (as a long-stored-in-void*) so
 * on_radae_text_rx can route the decoded string to the right slot. */
static rade_text_t g_rade_text_tx                 = NULL;
static rade_text_t g_rade_text_rx[RADAE_NRX]      = { NULL };

#define RADAE_OWN_CALL_CAP 16
static char g_tx_own_callsign[RADAE_OWN_CALL_CAP] = "";

/* TX-side mic peak / clip publication (single, TX is single). */
static volatile long g_tx_in_level_dbfs_q     = -120;
static volatile long g_tx_in_clip_last_tick   = 0;
#define RADAE_TX_CLIP_THRESHOLD 0.8f
#define RADAE_TX_CLIP_HOLD_MS   500

/* Diagnostic bypass flags (TX-side, single). */
static volatile long g_radae_bypass_micdsp    = 0;
static volatile long g_radae_bypass_core      = 0;
static volatile long g_radae_bypass_rmatch    = 0;
static volatile long g_radae_bypass_encoder   = 0;
static volatile long g_radae_bypass_all       = 0;

static int g_initialized = 0;

/* g_radae_cs -- shared RX-UI critical section.  Protects per-RX
 * remote-callsign state and the rade_text codecs.  Audio threads also
 * take it briefly while updating callsign / SNR.
 *
 * For minimum lock contention on the dual-RX decode hot path, the two
 * RX threads share one critical section but only hold it during state
 * publish moments (callsign decode, per-second diagnostics) -- the
 * heavy rade_rx + FARGAN work runs outside the lock. */
static CRITICAL_SECTION g_radae_cs;
static int              g_radae_cs_inited = 0;
static CRITICAL_SECTION g_radae_tx_cs;
static int              g_radae_tx_cs_inited = 0;

/* Per-RX radae library context. */
static struct rade*    g_rade[RADAE_NRX]                = { NULL };
static int             g_rade_n_tx_out                  = 0;
static int             g_rade_n_tx_eoo_out              = 0;
static int             g_rade_nin_max                   = 0;
static int             g_rade_n_features                = 0;
static int             g_rade_n_eoo_bits                = 0;

/* Opus DNN states.  FARGAN is per-RX (each decoder has its own
 * vocoder hidden state).  LPCNet encoder is TX-side single. */
static FARGANState     g_fargan[RADAE_NRX];
static LPCNetEncState* g_lpcnet_enc = NULL;
static int             g_opus_arch  = 0;

/* Per-RX RX-path scratch buffers. */
static int    g_rx_outrate_cached[RADAE_NRX]  = { 0 };
static int    g_rx_outsize_cached[RADAE_NRX]  = { 0 };
static void*  g_rx_resamp_down[RADAE_NRX]     = { NULL };
static void*  g_rx_resamp_up[RADAE_NRX]       = { NULL };
static float* g_rx_in_mono[RADAE_NRX]         = { NULL };
static float* g_rx_modem_8k[RADAE_NRX]        = { NULL };
static float* g_rx_speech_16k[RADAE_NRX]      = { NULL };
static float* g_rx_speech_outrate[RADAE_NRX]  = { NULL };
static float* g_rx_modem_fifo[RADAE_NRX]      = { NULL };
static int    g_rx_modem_fifo_n[RADAE_NRX]    = { 0 };
static int    g_rx_modem_fifo_cap[RADAE_NRX]  = { 0 };
static float* g_rx_speech_fifo[RADAE_NRX]     = { NULL };
static int    g_rx_speech_fifo_n[RADAE_NRX]   = { 0 };
static int    g_rx_speech_fifo_cap[RADAE_NRX] = { 0 };
static float* g_rx_outrate_fifo[RADAE_NRX]    = { NULL };
static int    g_rx_outrate_fifo_n[RADAE_NRX]  = { 0 };
static int    g_rx_outrate_fifo_cap[RADAE_NRX]= { 0 };
static float  g_rx_pending_features[RADAE_NRX][NB_TOTAL_FEATURES];
static int    g_rx_pending_features_n[RADAE_NRX] = { 0 };

/* Per-RX rade library scratch. */
static RADE_COMP* g_rade_rx_in[RADAE_NRX]       = { NULL };
static float*     g_rade_rx_features[RADAE_NRX] = { NULL };

/* TX path -- single. */
static int   g_tx_outrate_cached  = 0;
static int   g_tx_outsize_cached  = 0;
static void* g_tx_resamp_down     = NULL;
static void* g_tx_resamp_up       = NULL;
static void* g_tx_rmatch          = NULL;

static float* g_tx_in_mono        = NULL;
static float* g_tx_speech_16k     = NULL;
static float* g_tx_modem_8k       = NULL;
static float* g_tx_modem_outrate  = NULL;
static float* g_tx_speech_fifo    = NULL;
static int    g_tx_speech_fifo_n  = 0;
static int    g_tx_speech_fifo_cap= 0;
static float* g_tx_modem_fifo     = NULL;
static int    g_tx_modem_fifo_n   = 0;
static int    g_tx_modem_fifo_cap = 0;
static float* g_tx_outrate_fifo   = NULL;
static int    g_tx_outrate_fifo_n = 0;
static int    g_tx_outrate_fifo_cap = 0;
static float* g_tx_features_buf   = NULL;
static int    g_tx_features_buf_n = 0;

static double* g_tx_rmatch_in     = NULL;
static double* g_tx_rmatch_out    = NULL;
static int     g_tx_rmatch_blksz  = 0;

/* TX-side rade library scratch (single, TX is single). */
static RADE_COMP* g_rade_tx_out      = NULL;
static RADE_COMP* g_rade_tx_eoo_out  = NULL;
static float*     g_rade_eoo_bits    = NULL;

/* Per-RX loopback bridge.  When chkRADAELoopback[rx] is on, TX-side
 * modem audio is pushed into that RX's bridge instead of mic_io. */
#define RADAE_LOOP_BRIDGE_CAP 96000   /* 2 s at 48 kHz */
static float  g_loop_bridge[RADAE_NRX][RADAE_LOOP_BRIDGE_CAP];
static int    g_loop_bridge_n[RADAE_NRX]            = { 0 };
static long   g_loop_bridge_ovrun_count[RADAE_NRX]  = { 0 };
static long   g_loop_bridge_underrun_count[RADAE_NRX] = { 0 };

/* Diagnostic counters (rate-limited). */
static long g_tx_speech_ovrun_count   = 0;
static long g_tx_modem_ovrun_count    = 0;
static long g_tx_outrate_ovrun_count  = 0;
static long g_tx_outrate_underrun_count = 0;
static long g_rx_ovrun_count[RADAE_NRX] = { 0 };
static int  g_rx_diag_counter[RADAE_NRX] = { 0 };

#define RADAE_MAX_BLOCK      4096
#define RADAE_MAX_RESAMP_OUT (RADAE_MAX_BLOCK * 6 + 256)

/* ============================================================
 * FIFO helpers.
 * ============================================================ */

static void fifo_push_check(float* buf, int* n, int cap, const float* src, int count,
                            long* counter, const char* tag)
{
    int avail = cap - *n;
    int take  = (count < avail) ? count : avail;
    if (take > 0)
    {
        memcpy(buf + *n, src, take * sizeof(float));
        *n += take;
    }
    if (take < count)
    {
        long c = ++(*counter);
        if (c == 1 || (c % 50) == 0)
        {
            char log[160];
            sprintf_s(log, sizeof(log),
                "[RADAE] %s OVRUN dropped=%d total=%ld\n",
                tag, count - take, c);
            OutputDebugStringA(log);
        }
    }
}

static void fifo_pop(float* buf, int* n, float* dst, int count)
{
    if (count <= 0 || count > *n) return;
    memcpy(dst, buf, count * sizeof(float));
    *n -= count;
    if (*n > 0) memmove(buf, buf + count, (*n) * sizeof(float));
}

/* ============================================================
 * Resampler lifecycle
 * ============================================================ */

static void rebuild_rx_resamplers(int rx, int new_outrate)
{
    if (rx < 0 || rx >= RADAE_NRX) return;
    if (g_rx_resamp_down[rx]) { destroy_resampleFV(g_rx_resamp_down[rx]); g_rx_resamp_down[rx] = NULL; }
    if (g_rx_resamp_up[rx])   { destroy_resampleFV(g_rx_resamp_up[rx]);   g_rx_resamp_up[rx]   = NULL; }
    g_rx_resamp_down[rx] = create_resampleFV(new_outrate, RADE_MODEM_SAMPLE_RATE);
    g_rx_resamp_up[rx]   = create_resampleFV(RADE_SPEECH_SAMPLE_RATE, new_outrate);
    g_rx_outrate_cached[rx] = new_outrate;
}

static void rebuild_tx_resamplers(int new_outrate, int outsize)
{
    if (g_tx_resamp_down) { destroy_resampleFV(g_tx_resamp_down); g_tx_resamp_down = NULL; }
    if (g_tx_resamp_up)   { destroy_resampleFV(g_tx_resamp_up);   g_tx_resamp_up   = NULL; }
    if (g_tx_rmatch)      { destroy_rmatchV(g_tx_rmatch);         g_tx_rmatch      = NULL; }

    g_tx_resamp_down = create_resampleFV(new_outrate, RADE_SPEECH_SAMPLE_RATE);
    g_tx_resamp_up   = create_resampleFV(RADE_MODEM_SAMPLE_RATE, new_outrate);

    g_tx_rmatch_blksz = (outsize > 0 && outsize <= RADAE_MAX_BLOCK) ? outsize : 64;
    g_tx_rmatch = create_rmatchV(g_tx_rmatch_blksz,
                                 g_tx_rmatch_blksz,
                                 new_outrate, new_outrate,
                                 4096, 1.0);
    if (g_tx_rmatch != NULL)
        resetRMatchDiags(g_tx_rmatch);

    g_tx_outrate_cached = new_outrate;
    g_tx_outsize_cached = outsize;

    {
        char log[160];
        sprintf_s(log, sizeof(log),
            "[RADAE] TX resamplers built outrate=%d outsize=%d rmatch_blk=%d\n",
            new_outrate, outsize, g_tx_rmatch_blksz);
        OutputDebugStringA(log);
    }
}

/* ============================================================
 * rade_text callback: fires on every successful LDPC+CRC decode of
 * an EOO frame.  state carries the rx index (cast through void*) so
 * we know which receiver's callsign slot to update.
 * ============================================================ */

static void on_radae_text_rx(rade_text_t rt, const char* txt_ptr, int length, void* state)
{
    (void)rt;
    if (txt_ptr == NULL || length <= 0) return;
    /* The void* state slot encodes the rx index (0 or 1).  Stored as
     * (void*)(intptr_t)rx in rade_text_set_rx_callback. */
    int rx = (int)(intptr_t)state;
    if (rx < 0 || rx >= RADAE_NRX) return;
    if (g_radae_cs_inited) EnterCriticalSection(&g_radae_cs);
    {
        int n = length;
        if (n >= RADAE_REMOTE_CALL_CAP) n = RADAE_REMOTE_CALL_CAP - 1;
        memcpy(g_rx_remote_callsign[rx], txt_ptr, (size_t)n);
        g_rx_remote_callsign[rx][n] = '\0';
        _InterlockedIncrement(&g_rx_remote_callsign_seq[rx]);
        _InterlockedExchange(&g_eoo_decode_last_tick[rx], (long)GetTickCount());
        {
            char log[140];
            sprintf_s(log, sizeof(log),
                "[RADAE] RX%d rade_text decoded callsign: '%s' (len=%d)\n",
                rx + 1, g_rx_remote_callsign[rx], n);
            OutputDebugStringA(log);
        }
    }
    if (g_radae_cs_inited) LeaveCriticalSection(&g_radae_cs);
}

/* ============================================================
 * Lifecycle (called from create_pipe / destroy_pipe)
 * ============================================================ */

void create_radae(void)
{
    if (g_initialized) return;

    if (!g_radae_cs_inited)
    {
        InitializeCriticalSectionAndSpinCount(&g_radae_cs, 4000);
        g_radae_cs_inited = 1;
    }
    if (!g_radae_tx_cs_inited)
    {
        InitializeCriticalSectionAndSpinCount(&g_radae_tx_cs, 4000);
        g_radae_tx_cs_inited = 1;
    }

    rade_initialize();

    int rx;
    int any_ok = 0;
    for (rx = 0; rx < RADAE_NRX; rx++)
    {
        g_rade[rx] = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
        if (g_rade[rx] == NULL)
        {
            char msg[80];
            sprintf_s(msg, sizeof(msg),
                "[RADAE] rade_open() failed for RX%d; that receiver's RADE path will be unavailable.\n",
                rx + 1);
            OutputDebugStringA(msg);
        }
        else
        {
            any_ok = 1;
        }
    }
    if (!any_ok) return;

    /* All instances share the same geometry constants; query from the
     * first non-NULL context. */
    for (rx = 0; rx < RADAE_NRX; rx++)
    {
        if (g_rade[rx] != NULL)
        {
            g_rade_n_tx_out      = rade_n_tx_out(g_rade[rx]);
            g_rade_n_tx_eoo_out  = rade_n_tx_eoo_out(g_rade[rx]);
            g_rade_nin_max       = rade_nin_max(g_rade[rx]);
            g_rade_n_features    = rade_n_features_in_out(g_rade[rx]);
            g_rade_n_eoo_bits    = rade_n_eoo_bits(g_rade[rx]);
            break;
        }
    }

    g_opus_arch  = opus_select_arch();
    {
        float zeros_pcm[LPCNET_FRAME_SIZE];
        float zeros_feats[NB_TOTAL_FEATURES];
        int i;
        for (i = 0; i < LPCNET_FRAME_SIZE; i++)   zeros_pcm[i]   = 0.0f;
        for (i = 0; i < NB_TOTAL_FEATURES; i++)   zeros_feats[i] = 0.0f;
        for (rx = 0; rx < RADAE_NRX; rx++)
        {
            fargan_init(&g_fargan[rx]);
            fargan_cont(&g_fargan[rx], zeros_pcm, zeros_feats);
        }
    }
    g_lpcnet_enc = lpcnet_encoder_create();

    {
        const int MAX_BLOCK = RADAE_MAX_BLOCK;
        const int MAX_RESAMP_OUT = RADAE_MAX_RESAMP_OUT;

        for (rx = 0; rx < RADAE_NRX; rx++)
        {
            g_rx_in_mono[rx]        = (float*)_aligned_malloc(sizeof(float) * MAX_BLOCK,        16);
            g_rx_modem_8k[rx]       = (float*)_aligned_malloc(sizeof(float) * MAX_RESAMP_OUT,   16);
            g_rx_speech_16k[rx]     = (float*)_aligned_malloc(sizeof(float) * LPCNET_FRAME_SIZE * 8, 16);
            g_rx_speech_outrate[rx] = (float*)_aligned_malloc(sizeof(float) * MAX_RESAMP_OUT,   16);

            g_rx_modem_fifo_cap[rx]  = g_rade_nin_max * 4 + 2048;
            g_rx_speech_fifo_cap[rx] = MAX_RESAMP_OUT * 2 + LPCNET_FRAME_SIZE * 8;
            g_rx_modem_fifo[rx]      = (float*)_aligned_malloc(sizeof(float) * g_rx_modem_fifo_cap[rx],  16);
            g_rx_speech_fifo[rx]     = (float*)_aligned_malloc(sizeof(float) * g_rx_speech_fifo_cap[rx], 16);

            g_rx_outrate_fifo_cap[rx] = 32768;
            g_rx_outrate_fifo[rx]     = (float*)_aligned_malloc(sizeof(float) * g_rx_outrate_fifo_cap[rx], 16);

            g_rade_rx_in[rx]       = (RADE_COMP*)_aligned_malloc(sizeof(RADE_COMP) * g_rade_nin_max,    16);
            g_rade_rx_features[rx] = (float*)    _aligned_malloc(sizeof(float)     * g_rade_n_features, 16);

            g_rx_modem_fifo_n[rx]       = 0;
            g_rx_speech_fifo_n[rx]      = 0;
            g_rx_outrate_fifo_n[rx]     = 0;
            g_rx_pending_features_n[rx] = 0;
            g_rx_diag_counter[rx]       = 0;
            g_loop_bridge_n[rx]         = 0;
        }

        g_tx_in_mono        = (float*)_aligned_malloc(sizeof(float) * MAX_BLOCK,        16);
        g_tx_speech_16k     = (float*)_aligned_malloc(sizeof(float) * MAX_RESAMP_OUT,   16);
        g_tx_modem_8k       = (float*)_aligned_malloc(sizeof(float) * (g_rade_n_tx_out + g_rade_n_tx_eoo_out + 8000), 16);
        g_tx_modem_outrate  = (float*)_aligned_malloc(sizeof(float) * MAX_RESAMP_OUT,   16);

        g_tx_speech_fifo_cap = MAX_RESAMP_OUT * 2 + LPCNET_FRAME_SIZE * 8;
        g_tx_modem_fifo_cap  = (g_rade_n_tx_out * 4) + g_rade_n_tx_eoo_out + 8000;
        g_tx_speech_fifo     = (float*)_aligned_malloc(sizeof(float) * g_tx_speech_fifo_cap, 16);
        g_tx_modem_fifo      = (float*)_aligned_malloc(sizeof(float) * g_tx_modem_fifo_cap,  16);

        g_tx_outrate_fifo_cap = 65536;
        g_tx_outrate_fifo     = (float*)_aligned_malloc(sizeof(float) * g_tx_outrate_fifo_cap, 16);

        g_tx_features_buf    = (float*)_aligned_malloc(sizeof(float) * g_rade_n_features,    16);
        g_tx_features_buf_n  = 0;

        g_tx_rmatch_in   = (double*)_aligned_malloc(sizeof(double) * 2 * MAX_BLOCK, 16);
        g_tx_rmatch_out  = (double*)_aligned_malloc(sizeof(double) * 2 * MAX_BLOCK, 16);
    }

    /* TX-side rade library scratch (single). */
    g_rade_tx_out      = (RADE_COMP*)_aligned_malloc(sizeof(RADE_COMP) * g_rade_n_tx_out,     16);
    g_rade_tx_eoo_out  = (RADE_COMP*)_aligned_malloc(sizeof(RADE_COMP) * g_rade_n_tx_eoo_out, 16);
    g_rade_eoo_bits    = (float*)    _aligned_malloc(sizeof(float)     * g_rade_n_eoo_bits,   16);

    g_tx_speech_fifo_n = 0;
    g_tx_modem_fifo_n  = 0;
    g_tx_outrate_fifo_n = 0;
    g_tx_features_buf_n = 0;

    /* FreeDV-GUI rade_text codecs: one TX instance + one per RX.
     * The RX callback's state slot carries the rx index so the same
     * on_radae_text_rx function routes decoded callsigns to the right
     * per-RX slot. */
    if (g_rade_text_tx == NULL)
    {
        g_rade_text_tx = rade_text_create();
        /* TX codec has no RX callback registered (encoder-only use). */
    }
    for (rx = 0; rx < RADAE_NRX; rx++)
    {
        if (g_rade_text_rx[rx] == NULL)
        {
            g_rade_text_rx[rx] = rade_text_create();
            if (g_rade_text_rx[rx] != NULL)
                rade_text_set_rx_callback(g_rade_text_rx[rx], on_radae_text_rx,
                                          (void*)(intptr_t)rx);
        }
    }

    radae_micdsp_create(48000);

    g_initialized = 1;
    OutputDebugStringA("[RADAE] create_radae complete (dual-RX)\n");
}

void destroy_radae(void)
{
    if (!g_initialized) return;

    int rx;
    for (rx = 0; rx < RADAE_NRX; rx++)
        _InterlockedExchange(&g_radae_rx_enabled[rx], 0);
    _InterlockedExchange(&g_radae_tx_enabled, 0);

    radae_micdsp_destroy();

    EnterCriticalSection(&g_radae_cs);

    for (rx = 0; rx < RADAE_NRX; rx++)
    {
        if (g_rx_resamp_down[rx]) { destroy_resampleFV(g_rx_resamp_down[rx]); g_rx_resamp_down[rx] = NULL; }
        if (g_rx_resamp_up[rx])   { destroy_resampleFV(g_rx_resamp_up[rx]);   g_rx_resamp_up[rx]   = NULL; }

        if (g_rx_in_mono[rx])        { _aligned_free(g_rx_in_mono[rx]);        g_rx_in_mono[rx] = NULL; }
        if (g_rx_modem_8k[rx])       { _aligned_free(g_rx_modem_8k[rx]);       g_rx_modem_8k[rx] = NULL; }
        if (g_rx_speech_16k[rx])     { _aligned_free(g_rx_speech_16k[rx]);     g_rx_speech_16k[rx] = NULL; }
        if (g_rx_speech_outrate[rx]) { _aligned_free(g_rx_speech_outrate[rx]); g_rx_speech_outrate[rx] = NULL; }
        if (g_rx_modem_fifo[rx])     { _aligned_free(g_rx_modem_fifo[rx]);     g_rx_modem_fifo[rx] = NULL; }
        if (g_rx_speech_fifo[rx])    { _aligned_free(g_rx_speech_fifo[rx]);    g_rx_speech_fifo[rx] = NULL; }
        if (g_rx_outrate_fifo[rx])   { _aligned_free(g_rx_outrate_fifo[rx]);   g_rx_outrate_fifo[rx] = NULL; }

        if (g_rade_rx_in[rx])        { _aligned_free(g_rade_rx_in[rx]);        g_rade_rx_in[rx] = NULL; }
        if (g_rade_rx_features[rx])  { _aligned_free(g_rade_rx_features[rx]);  g_rade_rx_features[rx] = NULL; }

        if (g_rade_text_rx[rx]) { rade_text_destroy(g_rade_text_rx[rx]); g_rade_text_rx[rx] = NULL; }
        if (g_rade[rx])         { rade_close(g_rade[rx]);                g_rade[rx]         = NULL; }
    }

    if (g_tx_resamp_down) { destroy_resampleFV(g_tx_resamp_down); g_tx_resamp_down = NULL; }
    if (g_tx_resamp_up)   { destroy_resampleFV(g_tx_resamp_up);   g_tx_resamp_up   = NULL; }
    if (g_tx_rmatch)      { destroy_rmatchV(g_tx_rmatch);         g_tx_rmatch      = NULL; }

    if (g_tx_in_mono)        { _aligned_free(g_tx_in_mono);        g_tx_in_mono = NULL; }
    if (g_tx_speech_16k)     { _aligned_free(g_tx_speech_16k);     g_tx_speech_16k = NULL; }
    if (g_tx_modem_8k)       { _aligned_free(g_tx_modem_8k);       g_tx_modem_8k = NULL; }
    if (g_tx_modem_outrate)  { _aligned_free(g_tx_modem_outrate);  g_tx_modem_outrate = NULL; }
    if (g_tx_speech_fifo)    { _aligned_free(g_tx_speech_fifo);    g_tx_speech_fifo = NULL; }
    if (g_tx_modem_fifo)     { _aligned_free(g_tx_modem_fifo);     g_tx_modem_fifo = NULL; }
    if (g_tx_outrate_fifo)   { _aligned_free(g_tx_outrate_fifo);   g_tx_outrate_fifo = NULL; }
    if (g_tx_features_buf)   { _aligned_free(g_tx_features_buf);   g_tx_features_buf = NULL; }
    if (g_tx_rmatch_in)      { _aligned_free(g_tx_rmatch_in);      g_tx_rmatch_in = NULL; }
    if (g_tx_rmatch_out)     { _aligned_free(g_tx_rmatch_out);     g_tx_rmatch_out = NULL; }

    if (g_rade_tx_out)       { _aligned_free(g_rade_tx_out);       g_rade_tx_out = NULL; }
    if (g_rade_tx_eoo_out)   { _aligned_free(g_rade_tx_eoo_out);   g_rade_tx_eoo_out = NULL; }
    if (g_rade_eoo_bits)     { _aligned_free(g_rade_eoo_bits);     g_rade_eoo_bits = NULL; }

    if (g_lpcnet_enc) { lpcnet_encoder_destroy(g_lpcnet_enc); g_lpcnet_enc = NULL; }
    rade_finalize();
    if (g_rade_text_tx) { rade_text_destroy(g_rade_text_tx); g_rade_text_tx = NULL; }

    g_initialized = 0;

    LeaveCriticalSection(&g_radae_cs);
    DeleteCriticalSection(&g_radae_cs);
    g_radae_cs_inited = 0;
    if (g_radae_tx_cs_inited)
    {
        DeleteCriticalSection(&g_radae_tx_cs);
        g_radae_tx_cs_inited = 0;
    }
}

/* ============================================================
 * PORT exports for C#.  All RX-side getters/setters take an `int rx`
 * argument (0 = RX1, 1 = RX2).  TX-side and global PORTs stay
 * parameterless.  Out-of-range rx values are clamped or rejected.
 * ============================================================ */

static int radae_rx_valid(int rx) { return rx >= 0 && rx < RADAE_NRX; }

PORT void SetRadaeRxEnabled(int rx, int enable)
{
    if (!radae_rx_valid(rx)) return;
    _InterlockedExchange(&g_radae_rx_enabled[rx], enable ? 1 : 0);
    if (g_radae_cs_inited)
    {
        EnterCriticalSection(&g_radae_cs);
        g_rx_remote_callsign[rx][0] = '\0';
        LeaveCriticalSection(&g_radae_cs);
    }
}

PORT void SetRadaeTxEnabled(int enable)
{
    _InterlockedExchange(&g_radae_tx_enabled, enable ? 1 : 0);
}

PORT int GetRadaeRxEnabled(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    return (int)_InterlockedAnd(&g_radae_rx_enabled[rx], 0xffffffff);
}

PORT int GetRadaeTxEnabled(void)
{
    return (int)_InterlockedAnd(&g_radae_tx_enabled, 0xffffffff);
}

PORT int GetRadaeSync(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    return (int)_InterlockedAnd(&g_radae_sync[rx], 0xffffffff);
}

PORT int GetRadaeSnrDb(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    return (int)_InterlockedAnd(&g_radae_snr_db[rx], 0xffffffff);
}

PORT int GetRadaeRxLevelDb(int rx)
{
    if (!radae_rx_valid(rx)) return -120;
    return (int)_InterlockedAnd(&g_rx_in_level_dbfs_q[rx], 0xffffffff);
}

PORT int GetRadaeClip(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    long t = (long)_InterlockedAnd(&g_rx_in_clip_last_tick[rx], 0xffffffff);
    if (t == 0) return 0;
    long now = (long)GetTickCount();
    return ((now - t) >= 0 && (now - t) < RADAE_CLIP_HOLD_MS) ? 1 : 0;
}

PORT int GetRadaeRemoteCallsign(int rx, char* dst, int max)
{
    int n = 0;
    if (!radae_rx_valid(rx) || dst == NULL || max <= 0) return 0;
    if (!g_radae_cs_inited) { dst[0] = '\0'; return 0; }
    EnterCriticalSection(&g_radae_cs);
    {
        int len = (int)strlen(g_rx_remote_callsign[rx]);
        if (len >= max) len = max - 1;
        memcpy(dst, g_rx_remote_callsign[rx], (size_t)len);
        dst[len] = '\0';
        n = len;
    }
    LeaveCriticalSection(&g_radae_cs);
    return n;
}

PORT int GetRadaeRemoteCallsignSeq(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    return (int)_InterlockedAnd(&g_rx_remote_callsign_seq[rx], 0xffffffff);
}

PORT int GetRadaeEooDecodePulse(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    long t = (long)_InterlockedAnd(&g_eoo_decode_last_tick[rx], 0xffffffff);
    if (t == 0) return 0;
    long now = (long)GetTickCount();
    return ((now - t) >= 0 && (now - t) < RADAE_EOO_PULSE_MS) ? 1 : 0;
}

PORT int GetRadaeTxMicLevelDb(void)
{
    return (int)_InterlockedAnd(&g_tx_in_level_dbfs_q, 0xffffffff);
}

PORT int GetRadaeTxMicClip(void)
{
    long t = (long)_InterlockedAnd(&g_tx_in_clip_last_tick, 0xffffffff);
    if (t == 0) return 0;
    long now = (long)GetTickCount();
    return ((now - t) >= 0 && (now - t) < RADAE_TX_CLIP_HOLD_MS) ? 1 : 0;
}

PORT float GetRadaeFreqOffset(int rx)
{
    if (!radae_rx_valid(rx)) return 0.0f;
    return g_radae_freq_off[rx];
}

PORT void SetRadaeFreqOffset(int rx, float hz)
{
    if (!radae_rx_valid(rx)) return;
    g_radae_freq_off[rx] = hz;
}

PORT void RadaeNotifyEndOfOver(void)
{
    _InterlockedExchange(&g_radae_eoo_pending, 1);
    OutputDebugStringA("[RADAE] MOX 1->0 (EOO)\n");
}

PORT void RadaeNotifyBeginOver(void)
{
    _InterlockedExchange(&g_radae_box_pending, 1);
    /* fresh over: clear any stale EOO-flush handshake (arbiter owns silence-hold) */
    _InterlockedExchange(&g_radae_eoo_inflight, 0);
    _InterlockedExchange(&g_radae_eoo_flushed,  0);
    _InterlockedExchange(&g_radae_eoo_repeats_left, 0);
    OutputDebugStringA("[RADAE] MOX 0->1\n");
}

PORT int GetRadaeEooFlushed(void)
{
    return (int)_InterlockedAnd(&g_radae_eoo_flushed, 1);
}

PORT void SetRadaeTxSilenceHold(int on)
{
    _InterlockedExchange(&g_radae_tx_silence_hold, on ? 1 : 0);
}

PORT void SetRadaeLoopbackEnabled(int rx, int enable)
{
    if (!radae_rx_valid(rx)) return;
    long prev = _InterlockedExchange(&g_radae_loopback_enabled[rx], enable ? 1 : 0);
    if (!enable)
    {
        /* Drain that RX's bridge so the next loopback session starts clean. */
        g_loop_bridge_n[rx] = 0;
    }
    if (enable && !prev)
    {
        char log[80];
        sprintf_s(log, sizeof(log), "[RADAE] RX%d loopback START\n", rx + 1);
        OutputDebugStringA(log);
    }
    else if (!enable && prev)
    {
        char log[80];
        sprintf_s(log, sizeof(log), "[RADAE] RX%d loopback STOP\n", rx + 1);
        OutputDebugStringA(log);
    }
}

PORT int GetRadaeLoopbackEnabled(int rx)
{
    if (!radae_rx_valid(rx)) return 0;
    return (int)_InterlockedAnd(&g_radae_loopback_enabled[rx], 0xffffffff);
}

PORT void SetRadaeMoxState(int mox)
{
    const long new_state = mox ? 1 : 0;
    const long prev      = _InterlockedExchange(&g_radae_mox_state, new_state);
    if (prev && !new_state)
    {
        _InterlockedExchange(&g_radae_drain_blocks, 320);
        OutputDebugStringA("[RADAE] MOX 1->0 gate: drain window 320 blocks\n");
    }
    else if (!prev && new_state)
    {
        _InterlockedExchange(&g_radae_drain_blocks, 0);
        OutputDebugStringA("[RADAE] MOX 0->1 gate: TX path enabled\n");
    }
}

PORT void SetRadaeMicScale(double scale)
{
    if (!(scale > 0.0))      scale = 1.0;
    if (scale > 100.0)       scale = 100.0;
    g_radae_mic_scale = (float)scale;
}

PORT void SetRadaeRxScale(int rx, double scale)
{
    if (!radae_rx_valid(rx)) return;
    if (!(scale > 0.0))      scale = 1.0;
    if (scale > 100.0)       scale = 100.0;
    g_radae_rx_scale[rx] = (float)scale;
}

PORT void SetRadaeRxDialScale(int rx, double scale)
{
    if (!radae_rx_valid(rx)) return;
    if (!(scale > 0.0))      scale = 1.0;
    if (scale > 100.0)       scale = 100.0;
    g_radae_rx_dial_scale[rx] = (float)scale;
}

PORT void SetRadaeRxAFGain(int rx, double gain)
{
    if (!radae_rx_valid(rx)) return;
    if (!(gain >= 0.0))     gain = 0.0;
    if (gain > 100.0)       gain = 100.0;
    g_radae_rx_af_gain[rx] = (float)gain;
}

PORT float GetRadaeRxAFGain(int rx)
{
    if (!radae_rx_valid(rx)) return 1.0f;
    return g_radae_rx_af_gain[rx];
}

/* ============================================================
 * Pre-encoder mic-conditioning chain (TX-side, single).
 * ============================================================ */
PORT void SetRadaeMicRNNoiseEnabled(int e)        { radae_micdsp_set_rnnoise_enabled(e); }
PORT void SetRadaeMicAGCEnabled(int e)            { radae_micdsp_set_agc_enabled(e); }
PORT void SetRadaeMicAGCTargetLufs(double t)      { radae_micdsp_set_agc_target_lufs(t); }
PORT void SetRadaeMicEQEnabled(int e)             { radae_micdsp_set_eq_enabled(e); }
PORT void SetRadaeMicEQBass(double f, double g)   { radae_micdsp_set_eq_bass(f, g); }
PORT void SetRadaeMicEQMid(double f, double g, double q) { radae_micdsp_set_eq_mid(f, g, q); }
PORT void SetRadaeMicEQTreble(double f, double g) { radae_micdsp_set_eq_treble(f, g); }
PORT void SetRadaeMicEQVol(double db)             { radae_micdsp_set_eq_vol(db); }

/* ============================================================
 * Diagnostic bypass flags (TX-side, single).
 * ============================================================ */
PORT void SetRadaeBypassMicDsp(int enable)        { _InterlockedExchange(&g_radae_bypass_micdsp, enable ? 1 : 0); }
PORT void SetRadaeBypassEncoderCore(int enable)   { _InterlockedExchange(&g_radae_bypass_core,   enable ? 1 : 0); }
PORT void SetRadaeBypassRmatch(int enable)        { _InterlockedExchange(&g_radae_bypass_rmatch, enable ? 1 : 0); }
PORT void SetRadaeBypassEncoder(int enable)       { _InterlockedExchange(&g_radae_bypass_encoder, enable ? 1 : 0); }
PORT void SetRadaeBypassAll(int enable)           { _InterlockedExchange(&g_radae_bypass_all,    enable ? 1 : 0); }

PORT void SetRadaeEooCallsign(const char* callsign)
{
    if (callsign == NULL) return;
    EnterCriticalSection(&g_radae_cs);
    {
        size_t n = strlen(callsign);
        if (n >= RADAE_OWN_CALL_CAP) n = RADAE_OWN_CALL_CAP - 1;
        memcpy(g_tx_own_callsign, callsign, n);
        g_tx_own_callsign[n] = '\0';
    }
    LeaveCriticalSection(&g_radae_cs);
}

/* ============================================================
 * Hot-path: RX (post-WDSP demod -> speakers).  Per-rx dispatch.
 * ============================================================ */

void xradae_rx(int rx, double* rbuff_io)
{
    if (rx < 0 || rx >= RADAE_NRX) return;
    long en = _InterlockedAnd(&g_radae_rx_enabled[rx], 1);
    if (!en) return;
    if (!g_initialized || g_rade[rx] == NULL) return;
    if (rx >= pcm->cmRCVR) return;

    const int outrate = pcm->rcvr[rx].ch_outrate;
    const int outsize = pcm->rcvr[rx].ch_outsize;
    if (outrate <= 0 || outsize <= 0) return;

    /* MOX-state gating.  Skip the RX pipeline while the radio is in TX,
     * except when this RX has loopback enabled (chkRADAELoopback routes
     * the encoder output into the decoder input on that RX). */
    {
        const long mox = _InterlockedAnd(&g_radae_mox_state,                 1);
        const long lpb = _InterlockedAnd(&g_radae_loopback_enabled[rx],      1);
        if (mox && !lpb) return;
    }

    EnterCriticalSection(&g_radae_cs);

    if (g_rx_resamp_down[rx] == NULL || g_rx_resamp_up[rx] == NULL ||
        outrate != g_rx_outrate_cached[rx])
        rebuild_rx_resamplers(rx, outrate);
    g_rx_outsize_cached[rx] = outsize;

    /* 1) deswizzle L into mono float (or drain that RX's loopback bridge). */
    {
        int i;
        const int loopback_on = (int)_InterlockedAnd(&g_radae_loopback_enabled[rx], 0xffffffff);
        const float rx_scale  = g_radae_rx_scale[rx] * g_radae_rx_dial_scale[rx];
        float blk_peak = 0.0f;
        if (loopback_on)
        {
            int have = (g_loop_bridge_n[rx] < outsize) ? g_loop_bridge_n[rx] : outsize;
            if (have > 0)
            {
                memcpy(g_rx_in_mono[rx], g_loop_bridge[rx], (size_t)have * sizeof(float));
                g_loop_bridge_n[rx] -= have;
                if (g_loop_bridge_n[rx] > 0)
                    memmove(g_loop_bridge[rx], g_loop_bridge[rx] + have,
                            (size_t)g_loop_bridge_n[rx] * sizeof(float));
            }
            for (i = have; i < outsize; i++)
                g_rx_in_mono[rx][i] = 0.0f;
            if (have < outsize)
            {
                long c = ++g_loop_bridge_underrun_count[rx];
                if (c == 1 || (c % 50) == 0)
                {
                    char log[140];
                    sprintf_s(log, sizeof(log),
                        "[RADAE] RX%d loop_bridge UNDR have=%d need=%d total=%ld\n",
                        rx + 1, have, outsize, c);
                    OutputDebugStringA(log);
                }
            }
        }
        else
        {
            for (i = 0; i < outsize; i++)
                g_rx_in_mono[rx][i] = (float)rbuff_io[2 * i];
        }
        for (i = 0; i < outsize; i++)
        {
            float s = g_rx_in_mono[rx][i] * rx_scale;
            float a = (s < 0.0f) ? -s : s;
            if (a > blk_peak) blk_peak = a;
            g_rx_in_mono[rx][i] = s;
        }
        if (blk_peak > g_rx_in_peak[rx]) g_rx_in_peak[rx] = blk_peak;
        {
            float p = (blk_peak < 1e-9f) ? 1e-9f : blk_peak;
            int db = (int)(20.0f * (float)log10((double)p));
            if (db < -120) db = -120;
            if (db > 0)    db = 0;
            _InterlockedExchange(&g_rx_in_level_dbfs_q[rx], db);
        }
        if (blk_peak >= 0.8f)
            _InterlockedExchange(&g_rx_in_clip_last_tick[rx], (long)GetTickCount());
    }

    /* 2) downsample outrate -> 8 kHz, push into modem FIFO */
    {
        int n8 = 0;
        xresampleFV(g_rx_in_mono[rx], g_rx_modem_8k[rx], outsize, &n8, g_rx_resamp_down[rx]);
        if (n8 > 0)
            fifo_push_check(g_rx_modem_fifo[rx], &g_rx_modem_fifo_n[rx], g_rx_modem_fifo_cap[rx],
                            g_rx_modem_8k[rx], n8, &g_rx_ovrun_count[rx], "rx.modem_fifo");
    }

    /* 3) drain modem FIFO through rade_rx -> features -> FARGAN -> 16k speech FIFO */
    {
        int nin = rade_nin(g_rade[rx]);
        while (nin > 0 && nin <= g_rx_modem_fifo_n[rx])
        {
            int has_eoo = 0;
            int nout, i;
            float pop[8192];
            if (nin > (int)(sizeof(pop)/sizeof(pop[0]))) nin = (int)(sizeof(pop)/sizeof(pop[0]));
            fifo_pop(g_rx_modem_fifo[rx], &g_rx_modem_fifo_n[rx], pop, nin);

            for (i = 0; i < nin; i++)
            {
                g_rade_rx_in[rx][i].real = pop[i];
                g_rade_rx_in[rx][i].imag = 0.0f;
            }
            nout = rade_rx(g_rade[rx], g_rade_rx_features[rx], &has_eoo, g_rade_eoo_bits,
                           g_rade_rx_in[rx]);
            if (has_eoo)
            {
                if (g_rade_text_rx[rx] != NULL && g_rade_n_eoo_bits > 0)
                    rade_text_rx(g_rade_text_rx[rx], g_rade_eoo_bits, g_rade_n_eoo_bits / 2);
                g_rx_pending_features_n[rx] = 0;
            }
            else if (nout > 0)
            {
                int j;
                for (j = 0; j < nout; j++)
                {
                    g_rx_pending_features[rx][g_rx_pending_features_n[rx]++] = g_rade_rx_features[rx][j];
                    if (g_rx_pending_features_n[rx] == NB_TOTAL_FEATURES)
                    {
                        float fpcm[LPCNET_FRAME_SIZE];
                        fargan_synthesize(&g_fargan[rx], fpcm, g_rx_pending_features[rx]);
                        g_rx_pending_features_n[rx] = 0;
                        fifo_push_check(g_rx_speech_fifo[rx], &g_rx_speech_fifo_n[rx],
                                        g_rx_speech_fifo_cap[rx], fpcm, LPCNET_FRAME_SIZE,
                                        &g_rx_ovrun_count[rx], "rx.speech_fifo");
                    }
                }
            }

            _InterlockedExchange(&g_radae_sync[rx],   rade_sync(g_rade[rx]));
            _InterlockedExchange(&g_radae_snr_db[rx], rade_snrdB_3k_est(g_rade[rx]));

            nin = rade_nin(g_rade[rx]);
        }
    }

    /* 4) drain 16k speech FIFO -> upsample to outrate -> outrate FIFO */
    {
        const int SCRATCH_CAP_16K = LPCNET_FRAME_SIZE * 8;
        while (g_rx_speech_fifo_n[rx] > 0)
        {
            int can_take = g_rx_speech_fifo_n[rx];
            int nout = 0;
            if (can_take > SCRATCH_CAP_16K) can_take = SCRATCH_CAP_16K;
            fifo_pop(g_rx_speech_fifo[rx], &g_rx_speech_fifo_n[rx], g_rx_speech_16k[rx], can_take);
            xresampleFV(g_rx_speech_16k[rx], g_rx_speech_outrate[rx], can_take, &nout, g_rx_resamp_up[rx]);
            if (nout > 0)
                fifo_push_check(g_rx_outrate_fifo[rx], &g_rx_outrate_fifo_n[rx], g_rx_outrate_fifo_cap[rx],
                                g_rx_speech_outrate[rx], nout,
                                &g_rx_ovrun_count[rx], "rx.outrate_fifo");
        }
    }

    /* 5) drain outrate FIFO into rbuff_io; pad with silence on underrun */
    {
        int have = (g_rx_outrate_fifo_n[rx] < outsize) ? g_rx_outrate_fifo_n[rx] : outsize;
        int i;
        if (have > 0)
            fifo_pop(g_rx_outrate_fifo[rx], &g_rx_outrate_fifo_n[rx], g_rx_speech_outrate[rx], have);
        for (i = 0; i < have; i++)
        {
            double s = (double)g_rx_speech_outrate[rx][i];
            rbuff_io[2 * i]     = s;
            rbuff_io[2 * i + 1] = s;
        }
        for (; i < outsize; i++)
        {
            rbuff_io[2 * i]     = 0.0;
            rbuff_io[2 * i + 1] = 0.0;
        }
    }

    /* 6) Periodic RX sync/SNR (~once per second). */
    if (++g_rx_diag_counter[rx] >= 750)
    {
        char buf[160];
        int sync = (int)_InterlockedAnd(&g_radae_sync[rx],   0xffffffff);
        int snr  = (int)_InterlockedAnd(&g_radae_snr_db[rx], 0xffffffff);
        float peak = g_rx_in_peak[rx];
        float lvl_db;
        int clip = (peak >= 0.8f) ? 1 : 0;
        if (peak < 1e-9f) peak = 1e-9f;
        lvl_db = 20.0f * (float)log10((double)peak);
        sprintf_s(buf, sizeof(buf),
            "[RADAE] RX%d sync=%d snr=%d dB lvl=%.1f dBFS clip=%d\n",
            rx + 1, sync, snr, lvl_db, clip);
        OutputDebugStringA(buf);
        g_rx_diag_counter[rx] = 0;
        g_rx_in_peak[rx] = 0.0f;
    }

    LeaveCriticalSection(&g_radae_cs);
}

/* ============================================================
 * Hot-path: TX (single).  See pre-dual-RX history for the bypass
 * checkbox behaviour and the rationale behind the MOX-state gate.
 * ============================================================ */

#define RADE_TX_SCALE  0.5f

void xradae_tx(double* mic_io)
{
    const int tx_in_id = inid(1, 0);
    const int outrate  = pcm ? pcm->xcm_inrate[tx_in_id]  : -1;
    const int outsize  = pcm ? pcm->xcm_insize[tx_in_id]  : -1;

    long en = _InterlockedAnd(&g_radae_tx_enabled, 1);
    if (!en) return;
    if (!g_initialized || g_rade[0] == NULL) return;
    if (outrate <= 0 || outsize <= 0) return;

    if (_InterlockedAnd(&g_radae_bypass_all, 1)) return;

    /* MOX-state gating.  Mirror of the RX-side gate.  Loopback override
     * passes through if EITHER RX has loopback enabled. */
    {
        const long mox  = _InterlockedAnd(&g_radae_mox_state,                 1);
        const long lpb0 = _InterlockedAnd(&g_radae_loopback_enabled[0],       1);
        const long lpb1 = _InterlockedAnd(&g_radae_loopback_enabled[1],       1);
        const long eoo  = _InterlockedAnd(&g_radae_eoo_pending,               1);
        const long hold = _InterlockedAnd(&g_radae_tx_silence_hold,           1);
        /* hold keeps the gate passing so step 6 writes silence (or remaining
         * modem samples) into mic_io for the whole keyed flush window -- the
         * arbiter keeps the radio keyed until the EOO has flushed, and live
         * mic must never leak on-air. */
        if (!mox && !lpb0 && !lpb1 && !eoo && !hold)
        {
            const long drain = _InterlockedAnd(&g_radae_drain_blocks, 0xffffffff);
            if (drain <= 0) return;
            _InterlockedDecrement(&g_radae_drain_blocks);
        }
    }

    EnterCriticalSection(&g_radae_tx_cs);

    if (_InterlockedExchange(&g_radae_box_pending, 0))
    {
        if (g_tx_rmatch != NULL) resetRMatchDiags(g_tx_rmatch);
    }

    if (g_tx_resamp_down == NULL || g_tx_resamp_up == NULL || g_tx_rmatch == NULL ||
        outrate != g_tx_outrate_cached || outsize != g_tx_outsize_cached)
    {
        rebuild_tx_resamplers(outrate, outsize);
    }

    const int bypass_micdsp  = (int)_InterlockedAnd(&g_radae_bypass_micdsp,  1);
    const int bypass_core    = (int)_InterlockedAnd(&g_radae_bypass_core,    1);
    const int bypass_rmatch  = (int)_InterlockedAnd(&g_radae_bypass_rmatch,  1);
    const int bypass_encoder = (int)_InterlockedAnd(&g_radae_bypass_encoder, 1);
    (void)bypass_rmatch;

    /* Silence-hold (PTTRADE arbiter): the radio is held keyed while the EOO
     * flushes.  Skip live-mic ingest + speech encode (steps 2-3) so ONLY the
     * EOO frame + trailing silence drain out -- otherwise the encoder keeps
     * refilling the modem/outrate FIFOs from live mic, the flush never
     * completes (no GetRadaeEooFlushed ack) and live voice would go on-air. */
    const long tx_hold = _InterlockedAnd(&g_radae_tx_silence_hold, 1);

    /* 1) deswizzle + scale + publish TX mic level / clip */
    {
        const float mic_scale = g_radae_mic_scale;
        float blk_peak = 0.0f;
        int i;
        for (i = 0; i < outsize; i++)
        {
            float s = (float)mic_io[2 * i] * mic_scale;
            float a = (s < 0.0f) ? -s : s;
            if (a > blk_peak) blk_peak = a;
            g_tx_in_mono[i] = s;
        }
        {
            float p = (blk_peak < 1e-9f) ? 1e-9f : blk_peak;
            int db = (int)(20.0f * (float)log10((double)p));
            if (db < -120) db = -120;
            if (db > 0)    db = 0;
            _InterlockedExchange(&g_tx_in_level_dbfs_q, db);
        }
        if (blk_peak >= RADAE_TX_CLIP_THRESHOLD)
            _InterlockedExchange(&g_tx_in_clip_last_tick, (long)GetTickCount());
    }

    /* 1b/2) mic DSP + r8brain down */
    {
        int n_dsp = bypass_micdsp ? outsize
                                  : radae_micdsp_process(g_tx_in_mono, outsize);
        if (bypass_encoder)
        {
            int i;
            for (i = 0; i < n_dsp; i++)
            {
                mic_io[2 * i]     = (double)g_tx_in_mono[i];
                mic_io[2 * i + 1] = 0.0;
            }
            for (; i < outsize; i++)
            {
                mic_io[2 * i]     = 0.0;
                mic_io[2 * i + 1] = 0.0;
            }
            LeaveCriticalSection(&g_radae_tx_cs);
            return;
        }
        if (n_dsp > 0 && !tx_hold)
        {
            int n16 = 0;
            xresampleFV(g_tx_in_mono, g_tx_speech_16k, n_dsp,
                        &n16, g_tx_resamp_down);
            if (n16 > 0)
                fifo_push_check(g_tx_speech_fifo, &g_tx_speech_fifo_n, g_tx_speech_fifo_cap,
                                g_tx_speech_16k, n16,
                                &g_tx_speech_ovrun_count, "tx.speech_fifo");
        }
    }

    /* 3) LPCNet -> rade_tx  (skipped while silence-hold: no new speech frames
     *    so the modem FIFO only carries the EOO and can drain to empty) */
    while (!tx_hold && g_tx_speech_fifo_n >= LPCNET_FRAME_SIZE)
    {
        float frame_f[LPCNET_FRAME_SIZE];
        opus_int16 frame_i16[LPCNET_FRAME_SIZE];
        float feats[NB_TOTAL_FEATURES];
        int i;

        fifo_pop(g_tx_speech_fifo, &g_tx_speech_fifo_n, frame_f, LPCNET_FRAME_SIZE);
        for (i = 0; i < LPCNET_FRAME_SIZE; i++)
        {
            float s = frame_f[i] * 32768.0f;
            if (s >  32767.0f) s =  32767.0f;
            if (s < -32768.0f) s = -32768.0f;
            frame_i16[i] = (opus_int16)s;
        }

        lpcnet_compute_single_frame_features(g_lpcnet_enc, frame_i16, feats, g_opus_arch);

        for (i = 0; i < NB_TOTAL_FEATURES; i++)
        {
            g_tx_features_buf[g_tx_features_buf_n++] = feats[i];
            if (g_tx_features_buf_n == g_rade_n_features)
            {
                g_tx_features_buf_n = 0;
                if (!bypass_core)
                {
                    int n_modem = rade_tx(g_rade[0], g_rade_tx_out, g_tx_features_buf);
                    int j;
                    if (n_modem > 0)
                    {
                        int avail = (g_rade_n_tx_out > n_modem) ? n_modem : g_rade_n_tx_out;
                        for (j = 0; j < avail; j++)
                            g_tx_modem_8k[j] = g_rade_tx_out[j].real * RADE_TX_SCALE;
                        fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                                        g_tx_modem_8k, avail,
                                        &g_tx_modem_ovrun_count, "tx.modem_fifo");
                    }
                }
                else
                {
                    int silence_n = g_rade_n_tx_out;
                    memset(g_tx_modem_8k, 0, (size_t)silence_n * sizeof(float));
                    fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                                    g_tx_modem_8k, silence_n,
                                    &g_tx_modem_ovrun_count, "tx.modem_fifo");
                }
            }
        }
    }

    /* 4) EOO handling -- schedule a burst of repeated EOO frames (double EOO).
     *    The callsign bits are generated once here; the frames themselves are
     *    emitted one per call in step 4b, paced by the outrate-FIFO drain, with
     *    the 60 ms trailing silence pushed after the final frame. */
    if (_InterlockedExchange(&g_radae_eoo_pending, 0))
    {
        if (g_rade_text_tx != NULL && g_tx_own_callsign[0] != '\0' &&
            g_rade_eoo_bits != NULL && g_rade_n_eoo_bits > 0)
        {
            if (g_radae_cs_inited) EnterCriticalSection(&g_radae_cs);
            rade_text_generate_tx_string(g_rade_text_tx,
                                         g_tx_own_callsign,
                                         (int)strlen(g_tx_own_callsign),
                                         g_rade_eoo_bits,
                                         g_rade_n_eoo_bits / 2);
            rade_tx_set_eoo_bits(g_rade[0], g_rade_eoo_bits);
            if (g_radae_cs_inited) LeaveCriticalSection(&g_radae_cs);
        }
        _InterlockedExchange(&g_radae_eoo_repeats_left, 2);   /* double EOO */
        /* mark EOO in-flight for the flushed-out-of-mic_io ack (step 6 tail) */
        _InterlockedExchange(&g_radae_eoo_inflight, 1);
        _InterlockedExchange(&g_radae_eoo_flushed,  0);
    }

    /* 4b) Emit one scheduled EOO frame when the outrate FIFO has drained enough
     *     to take another, so the repeated EOOs play back-to-back without
     *     overflowing the modem/outrate FIFOs.  The 60 ms trailing silence is
     *     pushed after the final frame. */
    if (_InterlockedAnd(&g_radae_eoo_repeats_left, 0xffffffff) > 0 &&
        g_tx_modem_fifo_n == 0 && g_tx_outrate_fifo_n < (outsize * 2))
    {
        int n_eoo = rade_tx_eoo(g_rade[0], g_rade_tx_eoo_out);
        {
            char log[160];
            sprintf_s(log, sizeof(log), "[RADAE] EOO emit: call='%s' n_eoo=%d left=%ld\n",
                      g_tx_own_callsign, n_eoo,
                      _InterlockedAnd(&g_radae_eoo_repeats_left, 0xffffffff));
            OutputDebugStringA(log);
        }
        int j;
        if (n_eoo > 0)
        {
            for (j = 0; j < n_eoo; j++)
                g_tx_modem_8k[j] = g_rade_tx_eoo_out[j].real * RADE_TX_SCALE;
            fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                            g_tx_modem_8k, n_eoo,
                            &g_tx_modem_ovrun_count, "tx.modem_fifo");
        }
        if (_InterlockedDecrement(&g_radae_eoo_repeats_left) == 0)
        {
            float zeros[480];
            memset(zeros, 0, sizeof(zeros));
            fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                            zeros, 480,
                            &g_tx_modem_ovrun_count, "tx.modem_fifo");
        }
    }

    /* 5) ENCODER OUTPUT RESAMPLER: 8 k modem -> outrate */
    {
        const int MODEM_POP_CAP = 1024;
        while (g_tx_modem_fifo_n > 0)
        {
            int take = g_tx_modem_fifo_n;
            int nout = 0;
            if (take > MODEM_POP_CAP) take = MODEM_POP_CAP;
            fifo_pop(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_8k, take);
            xresampleFV(g_tx_modem_8k, g_tx_modem_outrate, take,
                        &nout, g_tx_resamp_up);
            if (nout > 0)
                fifo_push_check(g_tx_outrate_fifo, &g_tx_outrate_fifo_n, g_tx_outrate_fifo_cap,
                                g_tx_modem_outrate, nout,
                                &g_tx_outrate_ovrun_count, "tx.outrate_fifo");
        }
    }

    /* 6) FIFO-drain into mic_io.  Loopback is per-RX -- when any RX
     *    has loopback enabled, push the modem audio into that RX's
     *    bridge; mic_io stays silent if either is on so the radio
     *    does not transmit during loopback. */
    {
        float scratch[RADAE_MAX_BLOCK];
        int have = 0;
        int i;
        if (g_tx_outrate_fifo_n >= outsize)
        {
            fifo_pop(g_tx_outrate_fifo, &g_tx_outrate_fifo_n, scratch, outsize);
            have = outsize;
        }
        else
        {
            long c = ++g_tx_outrate_underrun_count;
            if (c == 1 || (c % 50) == 0)
            {
                char log[140];
                sprintf_s(log, sizeof(log),
                    "[RADAE] outrate->mic_io UNDR have=%d need=%d total=%ld (zero-fed)\n",
                    g_tx_outrate_fifo_n, outsize, c);
                OutputDebugStringA(log);
            }
        }

        const long lpb0 = _InterlockedAnd(&g_radae_loopback_enabled[0], 0xffffffff);
        const long lpb1 = _InterlockedAnd(&g_radae_loopback_enabled[1], 0xffffffff);

        if (lpb0 || lpb1)
        {
            int r;
            for (r = 0; r < RADAE_NRX; r++)
            {
                long lpb = (r == 0) ? lpb0 : lpb1;
                if (!lpb) continue;
                int avail = RADAE_LOOP_BRIDGE_CAP - g_loop_bridge_n[r];
                int take  = (have < avail) ? have : avail;
                if (take > 0)
                {
                    memcpy(g_loop_bridge[r] + g_loop_bridge_n[r], scratch,
                           (size_t)take * sizeof(float));
                    g_loop_bridge_n[r] += take;
                }
                if (take < have)
                {
                    long c = ++g_loop_bridge_ovrun_count[r];
                    if (c == 1 || (c % 50) == 0)
                    {
                        char log[140];
                        sprintf_s(log, sizeof(log),
                            "[RADAE] RX%d loop_bridge OVRUN dropped=%d total=%ld\n",
                            r + 1, have - take, c);
                        OutputDebugStringA(log);
                    }
                }
            }
            for (i = 0; i < outsize; i++)
            {
                mic_io[2 * i]     = 0.0;
                mic_io[2 * i + 1] = 0.0;
            }
        }
        else
        {
            for (i = 0; i < have; i++)
            {
                mic_io[2 * i]     = (double)scratch[i];
                mic_io[2 * i + 1] = 0.0;
            }
            for (; i < outsize; i++)
            {
                mic_io[2 * i]     = 0.0;
                mic_io[2 * i + 1] = 0.0;
            }
        }
    }

    /* EOO-flushed ack: modem fifo empty and only a sub-block of trailing
     * silence left in the outrate fifo => the EOO + 60ms silence have left
     * mic_io.  The arbiter polls this via GetRadaeEooFlushed(). */
    if (_InterlockedAnd(&g_radae_eoo_inflight, 1) &&
        _InterlockedAnd(&g_radae_eoo_repeats_left, 0xffffffff) == 0 &&
        g_tx_modem_fifo_n == 0 && g_tx_outrate_fifo_n < outsize)
    {
        _InterlockedExchange(&g_radae_eoo_flushed,  1);
        _InterlockedExchange(&g_radae_eoo_inflight, 0);
        OutputDebugStringA("[RADAE] EOO flushed out of mic_io\n");
    }

    LeaveCriticalSection(&g_radae_tx_cs);
}
