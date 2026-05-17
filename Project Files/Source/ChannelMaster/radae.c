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

/* FreeDV-GUI's reliable-text codec — vendored under lib/freedv_text/.
 * Replaces radae_nopy's naive 7-bit-MSB rade_rx_get_eoo_callsign /
 * rade_tx_set_eoo_callsign which are NOT the on-air FreeDV-GUI 2.3.0
 * format.  This vendoring uses LDPC(112,56) + CRC8 + 6-bit charset
 * + Gray interleaver, matching the real wire format. */
#include "rade_text.h"

#include "radae_micdsp.h"

#include <math.h>

/* WDSP mono-float resampler -- still used on RX */
extern void* create_resampleFV(int in_rate, int out_rate);
extern void  xresampleFV(float* input, float* output, int numsamps,
                         int* outsamps, void* ptr);
extern void  destroy_resampleFV(void* ptr);

/* WDSP rmatchV (PORT-exported by wdsp.dll).  Same call signatures as
 * used by ivac.c::xvacIN.  We extern-declare locally rather than
 * including comm.h's header chain. */
extern __declspec(dllimport) void* create_rmatchV(int in_size, int out_size,
                                                  int nom_inrate, int nom_outrate,
                                                  int ringsize, double var);
extern __declspec(dllimport) void  destroy_rmatchV(void* p);
extern __declspec(dllimport) void  xrmatchIN(void* b, double* in);
extern __declspec(dllimport) void  xrmatchOUT(void* b, double* out);
extern __declspec(dllimport) void  getRMatchDiags(void* b, int* underflows, int* overflows,
                                                  double* var, int* ringsize, int* nring);
extern __declspec(dllimport) void  resetRMatchDiags(void* b);

/* WDSP dexp audring flush (PORT-exported by wdsp.dll, defined in
 * Source/wdsp/dexp.c).  Used at the MOX RX->TX edge to drop the 60 ms
 * of stale mic audio that the audring captured during RX time, so the
 * audring does not transmit pre-PTT mic audio for the first 60 ms of
 * every over. */
extern __declspec(dllimport) void  FlushDexpAudioDelay(int id);

/* ============================================================
 * State
 * ============================================================ */

static volatile long g_radae_rx_enabled       = 0;
static volatile long g_radae_tx_enabled       = 0;
static volatile long g_radae_eoo_pending      = 0;
static volatile long g_radae_box_pending      = 0;   /* RX->MOX edge flag */
static volatile long g_radae_loopback_enabled = 0;   /* divert rmatchV output -> RX xresampleFV input (chkRADAE+chkRADAELoopback both ON) */
static volatile long g_radae_sync             = 0;
static volatile long g_radae_snr_db           = 0;
static          float g_radae_freq_off   = 0.0f;

/* Linear gain factors at the RADE encoder input / decoder input.
 *   g_radae_rx_scale       -- driven by VAC1 RXGain spinner; existing
 *                             original wiring.  Forced to 1.0 (0 dB)
 *                             while chkRADAE is checked because the
 *                             VAC1 RXGain spinner is greyed and zeroed
 *                             on RADE enable, so this path is neutral
 *                             whenever the new dial is in use.
 *   g_radae_mic_scale      -- driven by the dedicated "RADE Mic level"
 *                             spinner in Setup -> Audio -> Options
 *                             (FreeDV-GUI's "Mic level" equivalent).
 *                             Applied at the encoder INPUT.
 *   g_radae_rx_dial_scale  -- driven by the dedicated "RADE Rx level"
 *                             spinner in Setup -> Audio -> Options.
 *                             Applied at the decoder INPUT, in series
 *                             with g_radae_rx_scale (deliberately
 *                             separate code -- the two multiplications
 *                             collapse to a single combined multiply
 *                             pre-loop for cache friendliness).
 *
 * Independent of VAC1 TXGain.  As of the VAC1/RADE separation pass,
 * VAC1 spinners drive ONLY the VAC1 path; RADE has its own dedicated
 * dials (g_radae_mic_scale, g_radae_rx_dial_scale).  The VAC1 spinners
 * are NOT greyed when RADE is enabled.  On the TX side,
 * CMSetTXAPanelGain1 short-circuits to xpanel.gain1 = 1.0 whenever
 * RADE TX is on, so VAC1 TX Gain cannot leak into the modem output
 * level via the legacy VACPreamp branch.
 *
 * Aligned 32-bit float reads/writes compile to a single MOV on x86 and
 * are torn-free at the hardware level for UI-rate updates -- a plain
 * volatile is sufficient. */
static volatile float g_radae_mic_scale      = 1.0f;
static volatile float g_radae_rx_scale       = 1.0f;
static volatile float g_radae_rx_dial_scale  = 1.0f;

/* Per-second peak |sample| of the radae decoder input (post-rx scale).
 * Reset after each rx_diag print so the line shows the last second's
 * worst case. */
static float g_rx_in_peak                    = 0.0f;

/* Most recent block peak as a dBFS integer.  Updated every audio block
 * (no per-second reset, no decay) so a polling meter sees a snappy
 * current-level reading.  -120 represents "no signal".  Bit-cast into
 * int32 via _InterlockedExchange so reads from other threads are torn-
 * free on 32-bit Win. */
static volatile long g_rx_in_level_dbfs_q    = -120;

/* Wall-clock tick (GetTickCount ms) of the most recent block whose peak
 * crossed the 0.8 fullscale clip threshold.  GetRadaeClip() returns 1
 * if it is within the last ~500 ms, giving a half-second visible
 * pulse on the meter per clip event. */
static volatile long g_rx_in_clip_last_tick  = 0;
#define RADAE_CLIP_HOLD_MS 500

/* Most-recent decoded remote callsign from an inbound EOO frame.
 * Written by xradae_rx (audio thread) under g_radae_cs; read by
 * GetRadaeRemoteCallsign (UI thread) under the same critical section.
 * Cleared (zero-length string) when no callsign has yet been decoded
 * since the master enable was toggled. */
#define RADAE_REMOTE_CALL_CAP 16   /* RADE_EOO_CALLSIGN_MAX (8) + null + slack */
static char g_rx_remote_callsign[RADAE_REMOTE_CALL_CAP] = "";

/* Increments on every successful EOO callsign decode.  Polled by the
 * UI / reporter so it can fire one rx_report per decode event. */
static volatile long g_rx_remote_callsign_seq = 0;

/* GetTickCount() ms of the most recent successful EOO decode.  Drives
 * the 500 ms pulse on the RADE EOO Decodes meter. */
static volatile long g_eoo_decode_last_tick   = 0;
#define RADAE_EOO_PULSE_MS 500

/* FreeDV-GUI rade_text codec state.  Created once in create_radae and
 * destroyed in destroy_radae; lives for the whole session.  TX uses
 * g_rade_text + rade_text_generate_tx_string + rade_tx_set_eoo_bits;
 * RX uses g_rade_text + rade_text_rx (which fires on_radae_text_rx
 * synchronously when LDPC parity AND CRC pass). */
static rade_text_t g_rade_text       = NULL;
/* Caller-supplied own-callsign: cached when SetRadaeEooCallsign is
 * called and re-encoded to EOO floats on every TX-side EOO emission. */
#define RADAE_OWN_CALL_CAP 16
static char g_tx_own_callsign[RADAE_OWN_CALL_CAP] = "";

/* TX-side mic peak / clip publication.  Updated per xradae_tx call
 * with the post-VAC1-TXGain mono peak (g_tx_in_mono).  Value matches
 * the semantic of FreeDV-GUI's "Frm Mic" scope tap. */
static volatile long g_tx_in_level_dbfs_q     = -120;
static volatile long g_tx_in_clip_last_tick   = 0;
#define RADAE_TX_CLIP_THRESHOLD 0.8f       /* matches FreeDV's FROM_MIC_MAX */
#define RADAE_TX_CLIP_HOLD_MS   500

/* Diagnostic bypass flags -- runtime-toggleable atomic flags read at
 * the top of every xradae_tx() call.  Each flag short-circuits one
 * stage in the TX pipeline so we can isolate which stage is producing
 * the residual skirt splatter / receiver-decoder clicking.
 *
 *   g_radae_bypass_micdsp     -- skip Stage 1b (RNNoise/AGC/EQ);
 *                                 g_tx_in_mono passes through unchanged.
 *   g_radae_bypass_core       -- skip Stage 4 (rade_tx()); push silence
 *                                 (zero samples) into the modem FIFO so
 *                                 Stages 6 and 7 still run, but on
 *                                 silence.  Isolates the OFDM output.
 *   g_radae_bypass_rmatch     -- skip Stage 7 (rmatchV); copy the
 *                                 outrate FIFO directly into mic_io.
 *   g_radae_bypass_encoder    -- skip Stages 2..7 entirely; write the
 *                                 (optionally micdsp'd) mic_in_mono
 *                                 directly to mic_io as L=R 48 kHz
 *                                 stereo doubles.  Sends raw SSB voice
 *                                 over the air (NOT a RADE signal);
 *                                 single decisive bisect of whether the
 *                                 cause is inside xradae_tx at all.
 *
 * All flags default OFF.  Boot-OFF is enforced by the C# side: the four
 * checkboxes that drive these flags are non-persistent (plain CheckBox,
 * not CheckBoxTS) so they are unchecked every time the app starts. */
static volatile long g_radae_bypass_micdsp    = 0;
static volatile long g_radae_bypass_core      = 0;
static volatile long g_radae_bypass_rmatch    = 0;
static volatile long g_radae_bypass_encoder   = 0;

/* Test G -- coarsest bypass.  When this is set, xradae_tx() returns at
 * the very top of the function, before any read or write of mic_io.
 * Effectively makes xradae_tx a no-op while chkRADAE itself stays
 * on, so all chkRADAE side-effects on the rest of the application
 * (mode forced to DIGU/DIGL, VAC1 disabled, VACPreamp=1.0 in
 * CMSetTXAPanelGain1, xradae_rx running continuously on the RX path,
 * RADE Reporter background polling, etc.) remain in effect.  Isolates
 * "anything xradae_tx itself touches" from "anything else chkRADAE
 * activates application-wide". */
static volatile long g_radae_bypass_all       = 0;

static int g_initialized = 0;

/* g_radae_cs -- "shared" critical section.  Protects state accessed by
 * BOTH xradae_rx (RX audio thread) and the UI thread, OR by both audio
 * threads simultaneously.  Concretely: g_rx_remote_callsign and the
 * rade_text codec.  UI-thread setters that mutate UI-published state
 * also take it briefly.
 *
 * Originally this lock was ALSO held across the entire xradae_tx body.
 * That was a bug: xradae_rx holds the lock across rade_rx + FARGAN
 * synthesis -- a multi-millisecond CPU burst per RX audio block.  When
 * xradae_tx tried to acquire it on the next TX audio block, it waited
 * past its 1.33 ms TX-block deadline (at outsize=64 @ 48 k), the audio
 * engine produced a glitch, and the SSB modulator turned that into a
 * broadband transient on the RF skirts (the "bumps").
 *
 * xradae_tx and xradae_rx access disjoint state (separate FIFOs,
 * separate resamplers, separate rmatchV ring, separate sub-structs of
 * the rade context: r->tx vs r->rx).  So the lock split below is
 * structurally safe: TX uses g_radae_tx_cs only, RX continues to use
 * g_radae_cs.  The one piece of state they DO share -- the rade_text
 * EOO codec -- is touched briefly in xradae_tx step 4 and is wrapped
 * in g_radae_cs for those few calls. */
static CRITICAL_SECTION g_radae_cs;
static int              g_radae_cs_inited = 0;
static CRITICAL_SECTION g_radae_tx_cs;
static int              g_radae_tx_cs_inited = 0;

/* radae library context */
static struct rade*    g_rade   = NULL;
static int             g_rade_n_tx_out         = 0;
static int             g_rade_n_tx_eoo_out     = 0;
static int             g_rade_nin_max          = 0;
static int             g_rade_n_features       = 0;
static int             g_rade_n_eoo_bits       = 0;

/* Opus DNN states */
static FARGANState     g_fargan;
static LPCNetEncState* g_lpcnet_enc = NULL;
static int             g_opus_arch  = 0;

/* RX path -- WDSP xresampleFV (unchanged) */
static int    g_rx_outrate_cached = 0;
static int    g_rx_outsize_cached = 0;
static void*  g_rx_resamp_down    = NULL;   /* outrate -> 8 kHz */
static void*  g_rx_resamp_up      = NULL;   /* 16 kHz  -> outrate */
static float* g_rx_in_mono        = NULL;
static float* g_rx_modem_8k       = NULL;
static float* g_rx_speech_16k     = NULL;
static float* g_rx_speech_outrate = NULL;
static float* g_rx_modem_fifo     = NULL;
static int    g_rx_modem_fifo_n   = 0;
static int    g_rx_modem_fifo_cap = 0;
static float* g_rx_speech_fifo    = NULL;
static int    g_rx_speech_fifo_n  = 0;
static int    g_rx_speech_fifo_cap= 0;
static float* g_rx_outrate_fifo   = NULL;
static int    g_rx_outrate_fifo_n = 0;
static int    g_rx_outrate_fifo_cap = 0;
static float  g_rx_pending_features[NB_TOTAL_FEATURES];
static int    g_rx_pending_features_n = 0;

/* TX path -- r8brain on both ends, rmatchV after the encoder-output r8brain */
static int        g_tx_outrate_cached  = 0;
static int        g_tx_outsize_cached  = 0;
static r8b_handle g_tx_resamp_down     = NULL;   /* r8brain: outrate -> 16 kHz */
static r8b_handle g_tx_resamp_up       = NULL;   /* r8brain: 8 kHz   -> outrate */
static void*      g_tx_rmatch          = NULL;   /* rmatchV: outrate -> outrate */

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

/* rmatchV scratch -- 2*outsize doubles (interleaved L=R), pre-allocated. */
static double* g_tx_rmatch_in     = NULL;
static double* g_tx_rmatch_out    = NULL;
static int     g_tx_rmatch_blksz  = 0;       /* in_size = out_size for rmatchV */

/* Loopback bridge.  When chkRADAE+chkRADAELoopback are both ON, the L
 * channel of rmatchV's output (= the modem audio that would normally
 * fill mic_io) is pushed into this 48 kHz mono float ring instead.
 * xradae_rx then drains it in place of the rbuff_io deswizzle so the
 * decoder's xresampleFV (48k -> 8k) sees the loopback content on its
 * 48 kHz input side. */
#define RADAE_LOOP_BRIDGE_CAP 96000   /* 2 s at 48 kHz */
static float  g_loop_bridge[RADAE_LOOP_BRIDGE_CAP];
static int    g_loop_bridge_n = 0;
static long   g_loop_bridge_ovrun_count    = 0;
static long   g_loop_bridge_underrun_count = 0;

/* radae library scratch -- declared file-static so they are only
 * allocated once via _aligned_malloc in create_radae(). */
static RADE_COMP* g_rade_tx_out      = NULL;
static RADE_COMP* g_rade_tx_eoo_out  = NULL;
static float*     g_rade_eoo_bits    = NULL;
static RADE_COMP* g_rade_rx_in       = NULL;
static float*     g_rade_rx_features = NULL;

/* Diagnostic counters (rate-limited, short prints) */
static long g_tx_speech_ovrun_count   = 0;
static long g_tx_modem_ovrun_count    = 0;
static long g_tx_outrate_ovrun_count  = 0;
static long g_tx_outrate_underrun_count = 0;
static int  g_rx_diag_counter         = 0;

/* MAX_BLOCK and MAX_RESAMP_OUT -- generous fixed-ceiling scratch sizes */
#define RADAE_MAX_BLOCK      4096
#define RADAE_MAX_RESAMP_OUT (RADAE_MAX_BLOCK * 6 + 256)

/* ============================================================
 * FIFO helpers.  fifo_push_check reports overruns via DebugView
 * with a short tag; rate-limited to avoid flooding.
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

static void rebuild_rx_resamplers(int new_outrate)
{
    if (g_rx_resamp_down) { destroy_resampleFV(g_rx_resamp_down); g_rx_resamp_down = NULL; }
    if (g_rx_resamp_up)   { destroy_resampleFV(g_rx_resamp_up);   g_rx_resamp_up   = NULL; }
    g_rx_resamp_down = create_resampleFV(new_outrate, RADE_MODEM_SAMPLE_RATE);
    g_rx_resamp_up   = create_resampleFV(RADE_SPEECH_SAMPLE_RATE, new_outrate);
    g_rx_outrate_cached = new_outrate;
}

static void rebuild_tx_resamplers(int new_outrate, int outsize)
{
    if (g_tx_resamp_down) { r8b_destroy(g_tx_resamp_down); g_tx_resamp_down = NULL; }
    if (g_tx_resamp_up)   { r8b_destroy(g_tx_resamp_up);   g_tx_resamp_up   = NULL; }
    if (g_tx_rmatch)      { destroy_rmatchV(g_tx_rmatch);  g_tx_rmatch      = NULL; }

    /* r8brain encoder-input: outrate -> 16 kHz speech.  max_in_len sized
     * for any reasonable mic block (well above outsize). */
    g_tx_resamp_down = r8b_create((double)new_outrate,
                                  (double)RADE_SPEECH_SAMPLE_RATE,
                                  RADAE_MAX_BLOCK);

    /* r8brain encoder-output: 8 kHz modem -> outrate.  max_in_len bounded
     * to 1024 modem samples per call (matches our drain cap). */
    g_tx_resamp_up = r8b_create((double)RADE_MODEM_SAMPLE_RATE,
                                (double)new_outrate,
                                1024);

    /* rmatchV outrate -> outrate at the mic block size.  Same arrangement
     * VAC uses on its IN path, just with both ends on the radio clock so
     * the adaptive ratio sits at ~1.000.  4096-complex ring (~85 ms at
     * 48 kHz). */
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
 * an EOO frame.  Runs on the audio thread (synchronously from
 * rade_text_rx).  Copies the decoded callsign into the shared buffer
 * under g_radae_cs.
 * ============================================================ */

static void on_radae_text_rx(rade_text_t rt, const char* txt_ptr, int length, void* state)
{
    (void)rt; (void)state;
    if (txt_ptr == NULL || length <= 0) return;
    if (g_radae_cs_inited)
    {
        EnterCriticalSection(&g_radae_cs);
    }
    {
        int n = length;
        if (n >= RADAE_REMOTE_CALL_CAP) n = RADAE_REMOTE_CALL_CAP - 1;
        memcpy(g_rx_remote_callsign, txt_ptr, (size_t)n);
        g_rx_remote_callsign[n] = '\0';
        _InterlockedIncrement(&g_rx_remote_callsign_seq);
        _InterlockedExchange(&g_eoo_decode_last_tick, (long)GetTickCount());
        {
            char log[120];
            sprintf_s(log, sizeof(log),
                "[RADAE] rade_text decoded callsign: '%s' (len=%d)\n",
                g_rx_remote_callsign, n);
            OutputDebugStringA(log);
        }
    }
    if (g_radae_cs_inited)
    {
        LeaveCriticalSection(&g_radae_cs);
    }
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
    g_rade = rade_open("", RADE_USE_C_ENCODER | RADE_USE_C_DECODER | RADE_VERBOSE_0);
    if (g_rade == NULL)
    {
        OutputDebugStringA("[RADAE] rade_open() failed; RADE will be unavailable.\n");
        return;
    }

    g_rade_n_tx_out      = rade_n_tx_out(g_rade);
    g_rade_n_tx_eoo_out  = rade_n_tx_eoo_out(g_rade);
    g_rade_nin_max       = rade_nin_max(g_rade);
    g_rade_n_features    = rade_n_features_in_out(g_rade);
    g_rade_n_eoo_bits    = rade_n_eoo_bits(g_rade);

    g_opus_arch  = opus_select_arch();
    fargan_init(&g_fargan);
    {
        float zeros_pcm[LPCNET_FRAME_SIZE];
        float zeros_feats[NB_TOTAL_FEATURES];
        int i;
        for (i = 0; i < LPCNET_FRAME_SIZE; i++)   zeros_pcm[i]   = 0.0f;
        for (i = 0; i < NB_TOTAL_FEATURES; i++)   zeros_feats[i] = 0.0f;
        fargan_cont(&g_fargan, zeros_pcm, zeros_feats);
    }
    g_lpcnet_enc = lpcnet_encoder_create();

    /* Per-direction working buffers and FIFOs. */
    {
        const int MAX_BLOCK = RADAE_MAX_BLOCK;
        const int MAX_RESAMP_OUT = RADAE_MAX_RESAMP_OUT;

        g_rx_in_mono        = (float*)_aligned_malloc(sizeof(float) * MAX_BLOCK,        16);
        g_rx_modem_8k       = (float*)_aligned_malloc(sizeof(float) * MAX_RESAMP_OUT,   16);
        g_rx_speech_16k     = (float*)_aligned_malloc(sizeof(float) * LPCNET_FRAME_SIZE * 8, 16);
        g_rx_speech_outrate = (float*)_aligned_malloc(sizeof(float) * MAX_RESAMP_OUT,   16);

        g_rx_modem_fifo_cap  = g_rade_nin_max * 4 + 2048;
        g_rx_speech_fifo_cap = MAX_RESAMP_OUT * 2 + LPCNET_FRAME_SIZE * 8;
        g_rx_modem_fifo      = (float*)_aligned_malloc(sizeof(float) * g_rx_modem_fifo_cap,  16);
        g_rx_speech_fifo     = (float*)_aligned_malloc(sizeof(float) * g_rx_speech_fifo_cap, 16);

        g_rx_outrate_fifo_cap = 32768;
        g_rx_outrate_fifo     = (float*)_aligned_malloc(sizeof(float) * g_rx_outrate_fifo_cap, 16);

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

        /* rmatchV scratch: 2 * MAX_BLOCK doubles (interleaved L=R), enough
         * for the largest possible block size we will ever see. */
        g_tx_rmatch_in   = (double*)_aligned_malloc(sizeof(double) * 2 * MAX_BLOCK, 16);
        g_tx_rmatch_out  = (double*)_aligned_malloc(sizeof(double) * 2 * MAX_BLOCK, 16);
    }

    /* radae library scratch -- sized at create from the cached rade
     * dimensions and aligned for SIMD access inside radae_nopy. */
    g_rade_tx_out      = (RADE_COMP*)_aligned_malloc(sizeof(RADE_COMP) * g_rade_n_tx_out,     16);
    g_rade_tx_eoo_out  = (RADE_COMP*)_aligned_malloc(sizeof(RADE_COMP) * g_rade_n_tx_eoo_out, 16);
    g_rade_eoo_bits    = (float*)    _aligned_malloc(sizeof(float)     * g_rade_n_eoo_bits,   16);
    g_rade_rx_in       = (RADE_COMP*)_aligned_malloc(sizeof(RADE_COMP) * g_rade_nin_max,      16);
    g_rade_rx_features = (float*)    _aligned_malloc(sizeof(float)     * g_rade_n_features,   16);

    g_rx_modem_fifo_n  = 0;
    g_rx_speech_fifo_n = 0;
    g_rx_outrate_fifo_n = 0;
    g_rx_pending_features_n = 0;
    g_tx_speech_fifo_n = 0;
    g_tx_modem_fifo_n  = 0;
    g_tx_outrate_fifo_n = 0;
    g_tx_features_buf_n = 0;
    g_rx_diag_counter   = 0;

    /* FreeDV-GUI rade_text codec: create context + register callback. */
    if (g_rade_text == NULL)
    {
        g_rade_text = rade_text_create();
        if (g_rade_text != NULL)
            rade_text_set_rx_callback(g_rade_text, on_radae_text_rx, NULL);
    }

    /* Pre-encoder mic DSP chain (RNNoise + AGC + 3-band biquad EQ).
     * Sample rate is the radio's input rate, queried lazily from the
     * first xradae_tx call -- but we call create here with a default
     * 48 kHz rate, then if the actual rate differs the radae_micdsp
     * resampler / RNNoise gate handles it (RNNoise only runs at 48k). */
    radae_micdsp_create(48000);

    g_initialized = 1;
    OutputDebugStringA("[RADAE] create_radae complete\n");
}

void destroy_radae(void)
{
    if (!g_initialized) return;

    _InterlockedExchange(&g_radae_rx_enabled, 0);
    _InterlockedExchange(&g_radae_tx_enabled, 0);

    radae_micdsp_destroy();

    EnterCriticalSection(&g_radae_cs);

    if (g_rx_resamp_down) { destroy_resampleFV(g_rx_resamp_down); g_rx_resamp_down = NULL; }
    if (g_rx_resamp_up)   { destroy_resampleFV(g_rx_resamp_up);   g_rx_resamp_up   = NULL; }
    if (g_tx_resamp_down) { r8b_destroy(g_tx_resamp_down);        g_tx_resamp_down = NULL; }
    if (g_tx_resamp_up)   { r8b_destroy(g_tx_resamp_up);          g_tx_resamp_up   = NULL; }
    if (g_tx_rmatch)      { destroy_rmatchV(g_tx_rmatch);         g_tx_rmatch      = NULL; }

    if (g_rx_in_mono)        { _aligned_free(g_rx_in_mono);        g_rx_in_mono = NULL; }
    if (g_rx_modem_8k)       { _aligned_free(g_rx_modem_8k);       g_rx_modem_8k = NULL; }
    if (g_rx_speech_16k)     { _aligned_free(g_rx_speech_16k);     g_rx_speech_16k = NULL; }
    if (g_rx_speech_outrate) { _aligned_free(g_rx_speech_outrate); g_rx_speech_outrate = NULL; }
    if (g_rx_modem_fifo)     { _aligned_free(g_rx_modem_fifo);     g_rx_modem_fifo = NULL; }
    if (g_rx_speech_fifo)    { _aligned_free(g_rx_speech_fifo);    g_rx_speech_fifo = NULL; }
    if (g_rx_outrate_fifo)   { _aligned_free(g_rx_outrate_fifo);   g_rx_outrate_fifo = NULL; }

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
    if (g_rade_rx_in)        { _aligned_free(g_rade_rx_in);        g_rade_rx_in = NULL; }
    if (g_rade_rx_features)  { _aligned_free(g_rade_rx_features);  g_rade_rx_features = NULL; }

    if (g_lpcnet_enc) { lpcnet_encoder_destroy(g_lpcnet_enc); g_lpcnet_enc = NULL; }
    if (g_rade)       { rade_close(g_rade);                   g_rade       = NULL; }
    rade_finalize();
    if (g_rade_text)  { rade_text_destroy(g_rade_text);        g_rade_text  = NULL; }

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
 * PORT exports for C#
 * ============================================================ */

PORT void SetRadaeRxEnabled(int enable)
{
    _InterlockedExchange(&g_radae_rx_enabled, enable ? 1 : 0);
    /* Drop any stale decoded callsign; next EOO will repopulate. */
    if (g_radae_cs_inited)
    {
        EnterCriticalSection(&g_radae_cs);
        g_rx_remote_callsign[0] = '\0';
        LeaveCriticalSection(&g_radae_cs);
    }
}

PORT void SetRadaeTxEnabled(int enable)
{
    _InterlockedExchange(&g_radae_tx_enabled, enable ? 1 : 0);
}

PORT int GetRadaeRxEnabled(void)
{
    return (int)_InterlockedAnd(&g_radae_rx_enabled, 0xffffffff);
}

PORT int GetRadaeTxEnabled(void)
{
    return (int)_InterlockedAnd(&g_radae_tx_enabled, 0xffffffff);
}

PORT int GetRadaeSync(void)
{
    return (int)_InterlockedAnd(&g_radae_sync, 0xffffffff);
}

PORT int GetRadaeSnrDb(void)
{
    return (int)_InterlockedAnd(&g_radae_snr_db, 0xffffffff);
}

PORT int GetRadaeRxLevelDb(void)
{
    return (int)_InterlockedAnd(&g_rx_in_level_dbfs_q, 0xffffffff);
}

PORT int GetRadaeClip(void)
{
    long t = (long)_InterlockedAnd(&g_rx_in_clip_last_tick, 0xffffffff);
    if (t == 0) return 0;
    long now = (long)GetTickCount();
    return ((now - t) >= 0 && (now - t) < RADAE_CLIP_HOLD_MS) ? 1 : 0;
}

PORT int GetRadaeRemoteCallsign(char* dst, int max)
{
    int n = 0;
    if (dst == NULL || max <= 0) return 0;
    if (!g_radae_cs_inited) { dst[0] = '\0'; return 0; }
    EnterCriticalSection(&g_radae_cs);
    {
        int len = (int)strlen(g_rx_remote_callsign);
        if (len >= max) len = max - 1;
        memcpy(dst, g_rx_remote_callsign, (size_t)len);
        dst[len] = '\0';
        n = len;
    }
    LeaveCriticalSection(&g_radae_cs);
    return n;
}

PORT int GetRadaeRemoteCallsignSeq(void)
{
    return (int)_InterlockedAnd(&g_rx_remote_callsign_seq, 0xffffffff);
}

PORT int GetRadaeEooDecodePulse(void)
{
    long t = (long)_InterlockedAnd(&g_eoo_decode_last_tick, 0xffffffff);
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

PORT float GetRadaeFreqOffset(void)
{
    return g_radae_freq_off;
}

PORT void SetRadaeFreqOffset(float hz)
{
    g_radae_freq_off = hz;
}

PORT void RadaeNotifyEndOfOver(void)
{
    _InterlockedExchange(&g_radae_eoo_pending, 1);
    OutputDebugStringA("[RADAE] MOX 1->0 (EOO)\n");
}

PORT void RadaeNotifyBeginOver(void)
{
    _InterlockedExchange(&g_radae_box_pending, 1);
    OutputDebugStringA("[RADAE] MOX 0->1\n");
}

PORT void SetRadaeLoopbackEnabled(int enable)
{
    long prev = _InterlockedExchange(&g_radae_loopback_enabled, enable ? 1 : 0);
    if (!enable)
    {
        /* Drain the bridge so the next loopback session starts clean.
         * Single-writer (UI thread) on a plain int -- the audio threads
         * read it via the enable flag, which is the gate. */
        g_loop_bridge_n = 0;
    }
    if (enable && !prev)
        OutputDebugStringA("[RADAE] loopback START\n");
    else if (!enable && prev)
        OutputDebugStringA("[RADAE] loopback STOP\n");
}

PORT int GetRadaeLoopbackEnabled(void)
{
    return (int)_InterlockedAnd(&g_radae_loopback_enabled, 0xffffffff);
}

PORT void SetRadaeMicScale(double scale)
{
    /* Clamp to a sane range; UI sliders cap well within this. */
    if (!(scale > 0.0))      scale = 1.0;
    if (scale > 100.0)       scale = 100.0;
    g_radae_mic_scale = (float)scale;
}

PORT void SetRadaeRxScale(double scale)
{
    if (!(scale > 0.0))      scale = 1.0;
    if (scale > 100.0)       scale = 100.0;
    g_radae_rx_scale = (float)scale;
}

/* Dedicated "RADE Rx level" dial (separate from VAC1 RXGain).
 * Applied as a second linear multiplier in xradae_rx step 1, in
 * series with g_radae_rx_scale.  Both writes are torn-free on x86,
 * the audio thread takes a snapshot of each volatile once per block
 * and combines them before the per-sample loop. */
PORT void SetRadaeRxDialScale(double scale)
{
    if (!(scale > 0.0))      scale = 1.0;
    if (scale > 100.0)       scale = 100.0;
    g_radae_rx_dial_scale = (float)scale;
}

/* ============================================================
 * Pre-encoder mic-conditioning chain (FreeDV-GUI parity).  Thin
 * forwarders to radae_micdsp.c.  Enable flags + parameters; the
 * mic chain is gated inside xradae_tx by g_radae_tx_enabled, so
 * these have effect only when RADE is enabled.
 * ============================================================ */
PORT void SetRadaeMicRNNoiseEnabled(int e)
{
    radae_micdsp_set_rnnoise_enabled(e);
}

PORT void SetRadaeMicAGCEnabled(int e)
{
    radae_micdsp_set_agc_enabled(e);
}

PORT void SetRadaeMicAGCTargetLufs(double t)
{
    radae_micdsp_set_agc_target_lufs(t);
}

PORT void SetRadaeMicEQEnabled(int e)
{
    radae_micdsp_set_eq_enabled(e);
}

PORT void SetRadaeMicEQBass(double f, double g)
{
    radae_micdsp_set_eq_bass(f, g);
}

PORT void SetRadaeMicEQMid(double f, double g, double q)
{
    radae_micdsp_set_eq_mid(f, g, q);
}

PORT void SetRadaeMicEQTreble(double f, double g)
{
    radae_micdsp_set_eq_treble(f, g);
}

PORT void SetRadaeMicEQVol(double db)
{
    radae_micdsp_set_eq_vol(db);
}

/* ============================================================
 * Diagnostic bypass flags.  Each setter takes 0 (= normal path) or
 * non-zero (= bypass that stage).  Flags are torn-free volatiles read
 * at the top of every xradae_tx() call.  See the comment block on the
 * g_radae_bypass_* globals near the top of this file for the full
 * description of what each flag bypasses and what test it isolates.
 *
 * The C# UI wires these to four non-persistent checkboxes under
 * Setup -> DSP -> RADE so a forgotten test flag cannot leak into a
 * normal QSO across restarts.
 * ============================================================ */
PORT void SetRadaeBypassMicDsp(int enable)
{
    _InterlockedExchange(&g_radae_bypass_micdsp, enable ? 1 : 0);
}

PORT void SetRadaeBypassEncoderCore(int enable)
{
    _InterlockedExchange(&g_radae_bypass_core, enable ? 1 : 0);
}

PORT void SetRadaeBypassRmatch(int enable)
{
    _InterlockedExchange(&g_radae_bypass_rmatch, enable ? 1 : 0);
}

PORT void SetRadaeBypassEncoder(int enable)
{
    _InterlockedExchange(&g_radae_bypass_encoder, enable ? 1 : 0);
}

PORT void SetRadaeBypassAll(int enable)
{
    _InterlockedExchange(&g_radae_bypass_all, enable ? 1 : 0);
}

/* Cache the user's callsign for EOO transmission.  The actual encode
 * (rade_text_generate_tx_string + rade_tx_set_eoo_bits) happens just
 * before each rade_tx_eoo() in xradae_tx so the callsign always
 * matches what the user has currently set.  Replaces the old
 * radae_nopy 7-bit-MSB rade_tx_set_eoo_callsign which is NOT the
 * on-air FreeDV-GUI 2.3.0 wire format. */
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
 * Hot-path: RX (post-WDSP demod -> speakers)
 * RX rate-conversion stays on WDSP xresampleFV (this revision only
 * changes the encoder-side resamplers).
 * ============================================================ */

void xradae_rx(int rx, double* rbuff_io)
{
    long en = _InterlockedAnd(&g_radae_rx_enabled, 1);
    if (!en) return;
    if (!g_initialized || g_rade == NULL) return;
    if (rx < 0 || rx >= pcm->cmRCVR) return;
    if (rx != 0) return;        /* RX1 only -- single rade context */

    const int outrate = pcm->rcvr[rx].ch_outrate;
    const int outsize = pcm->rcvr[rx].ch_outsize;
    if (outrate <= 0 || outsize <= 0) return;

    EnterCriticalSection(&g_radae_cs);

    if (g_rx_resamp_down == NULL || g_rx_resamp_up == NULL || outrate != g_rx_outrate_cached)
        rebuild_rx_resamplers(outrate);
    g_rx_outsize_cached = outsize;

    /* 1) deswizzle L into mono float.
     *    LOOPBACK: when chkRADAE+chkRADAELoopback are both ON, drain the
     *    loopback bridge instead of rbuff_io.  The bridge holds the L
     *    channel of rmatchV's output (= what would normally fill mic_io)
     *    so the decoder sees the same modem audio the encoder produced.
     *    Pad short reads with zeros (decoder treats as "no signal" until
     *    the bridge fills, then re-syncs).
     *    After deswizzle, multiply by g_radae_rx_scale (VAC1 RXGain) AND
     *    g_radae_rx_dial_scale (the dedicated "RADE Rx level" spinner)
     *    and track per-second peak |sample| for the rx_diag print.
     *    Two volatile snapshots, combined into one multiplier so the
     *    per-sample loop is the same MUL count as before.  When chkRADAE
     *    is on, the UI greys the VAC1 RXGain spinner and forces it to
     *    0 dB so g_radae_rx_scale = 1.0; the dial then carries the
     *    full effective gain.  When chkRADAE is off, both volatiles
     *    are still read but the surrounding xradae_rx return-early
     *    on !g_radae_rx_enabled means this code does not execute. */
    {
        int i;
        const int loopback_on = (int)_InterlockedAnd(&g_radae_loopback_enabled, 0xffffffff);
        const float rx_scale  = g_radae_rx_scale * g_radae_rx_dial_scale;
        float blk_peak = 0.0f;
        if (loopback_on)
        {
            int have = (g_loop_bridge_n < outsize) ? g_loop_bridge_n : outsize;
            if (have > 0)
            {
                memcpy(g_rx_in_mono, g_loop_bridge, (size_t)have * sizeof(float));
                g_loop_bridge_n -= have;
                if (g_loop_bridge_n > 0)
                    memmove(g_loop_bridge, g_loop_bridge + have,
                            (size_t)g_loop_bridge_n * sizeof(float));
            }
            for (i = have; i < outsize; i++)
                g_rx_in_mono[i] = 0.0f;
            if (have < outsize)
            {
                long c = ++g_loop_bridge_underrun_count;
                if (c == 1 || (c % 50) == 0)
                {
                    char log[120];
                    sprintf_s(log, sizeof(log),
                        "[RADAE] loop_bridge UNDR have=%d need=%d total=%ld\n",
                        have, outsize, c);
                    OutputDebugStringA(log);
                }
            }
        }
        else
        {
            for (i = 0; i < outsize; i++)
                g_rx_in_mono[i] = (float)rbuff_io[2 * i];
        }
        for (i = 0; i < outsize; i++)
        {
            float s = g_rx_in_mono[i] * rx_scale;
            float a = (s < 0.0f) ? -s : s;
            if (a > blk_peak) blk_peak = a;
            g_rx_in_mono[i] = s;
        }
        if (blk_peak > g_rx_in_peak) g_rx_in_peak = blk_peak;
        /* Publish current block peak as dBFS for the meter. */
        {
            float p = (blk_peak < 1e-9f) ? 1e-9f : blk_peak;
            int db = (int)(20.0f * (float)log10((double)p));
            if (db < -120) db = -120;
            if (db > 0)    db = 0;
            _InterlockedExchange(&g_rx_in_level_dbfs_q, db);
        }
        /* Stamp clip-tick for the meter; same 0.8 threshold used by
         * the per-second rx_diag print so the two indicators agree. */
        if (blk_peak >= 0.8f)
            _InterlockedExchange(&g_rx_in_clip_last_tick, (long)GetTickCount());
    }

    /* 2) downsample outrate -> 8 kHz, push into modem FIFO */
    {
        int n8 = 0;
        xresampleFV(g_rx_in_mono, g_rx_modem_8k, outsize, &n8, g_rx_resamp_down);
        if (n8 > 0)
            fifo_push_check(g_rx_modem_fifo, &g_rx_modem_fifo_n, g_rx_modem_fifo_cap,
                            g_rx_modem_8k, n8, &g_tx_speech_ovrun_count, "rx.modem_fifo");
    }

    /* 3) drain modem FIFO through rade_rx -> features -> FARGAN -> 16k speech FIFO */
    {
        int nin = rade_nin(g_rade);
        while (nin > 0 && nin <= g_rx_modem_fifo_n)
        {
            int has_eoo = 0;
            int nout, i;
            float pop[8192];
            if (nin > (int)(sizeof(pop)/sizeof(pop[0]))) nin = (int)(sizeof(pop)/sizeof(pop[0]));
            fifo_pop(g_rx_modem_fifo, &g_rx_modem_fifo_n, pop, nin);

            /* Convert popped 8 kHz floats into RADE_COMP scratch then call rade_rx */
            for (i = 0; i < nin; i++)
            {
                g_rade_rx_in[i].real = pop[i];
                g_rade_rx_in[i].imag = 0.0f;
            }
            nout = rade_rx(g_rade, g_rade_rx_features, &has_eoo, g_rade_eoo_bits, g_rade_rx_in);
            if (has_eoo)
            {
                /* Decode the embedded callsign from the EOO soft-bits via
                 * FreeDV-GUI's rade_text codec.  The codec internally
                 * runs LDPC(112,56) + CRC8 verify; on success it
                 * synchronously fires on_radae_text_rx (registered in
                 * create_radae) which copies into g_rx_remote_callsign,
                 * bumps g_rx_remote_callsign_seq, and stamps
                 * g_eoo_decode_last_tick.  No callback fires when the
                 * decode fails, so a noisy frame is silently dropped --
                 * matches FreeDV-GUI's own behaviour.
                 *
                 * symSize is the count of (I,Q) symbol pairs, hence
                 * n_eoo_bits / 2. */
                if (g_rade_text != NULL && g_rade_n_eoo_bits > 0)
                    rade_text_rx(g_rade_text, g_rade_eoo_bits, g_rade_n_eoo_bits / 2);
                g_rx_pending_features_n = 0;
            }
            else if (nout > 0)
            {
                int j;
                for (j = 0; j < nout; j++)
                {
                    g_rx_pending_features[g_rx_pending_features_n++] = g_rade_rx_features[j];
                    if (g_rx_pending_features_n == NB_TOTAL_FEATURES)
                    {
                        float fpcm[LPCNET_FRAME_SIZE];
                        fargan_synthesize(&g_fargan, fpcm, g_rx_pending_features);
                        g_rx_pending_features_n = 0;
                        fifo_push_check(g_rx_speech_fifo, &g_rx_speech_fifo_n,
                                        g_rx_speech_fifo_cap, fpcm, LPCNET_FRAME_SIZE,
                                        &g_tx_modem_ovrun_count, "rx.speech_fifo");
                    }
                }
            }

            _InterlockedExchange(&g_radae_sync,   rade_sync(g_rade));
            _InterlockedExchange(&g_radae_snr_db, rade_snrdB_3k_est(g_rade));

            nin = rade_nin(g_rade);
        }
    }

    /* 4) drain 16k speech FIFO -> upsample to outrate -> outrate FIFO */
    {
        const int SCRATCH_CAP_16K = LPCNET_FRAME_SIZE * 8;
        while (g_rx_speech_fifo_n > 0)
        {
            int can_take = g_rx_speech_fifo_n;
            int nout = 0;
            if (can_take > SCRATCH_CAP_16K) can_take = SCRATCH_CAP_16K;
            fifo_pop(g_rx_speech_fifo, &g_rx_speech_fifo_n, g_rx_speech_16k, can_take);
            xresampleFV(g_rx_speech_16k, g_rx_speech_outrate, can_take, &nout, g_rx_resamp_up);
            if (nout > 0)
                fifo_push_check(g_rx_outrate_fifo, &g_rx_outrate_fifo_n, g_rx_outrate_fifo_cap,
                                g_rx_speech_outrate, nout,
                                &g_tx_outrate_ovrun_count, "rx.outrate_fifo");
        }
    }

    /* 5) drain outrate FIFO into rbuff_io; pad with silence on underrun
     *    (correct for "no signal yet" at the speaker). */
    {
        int have = (g_rx_outrate_fifo_n < outsize) ? g_rx_outrate_fifo_n : outsize;
        int i;
        if (have > 0)
            fifo_pop(g_rx_outrate_fifo, &g_rx_outrate_fifo_n, g_rx_speech_outrate, have);
        for (i = 0; i < have; i++)
        {
            double s = (double)g_rx_speech_outrate[i];
            rbuff_io[2 * i]     = s;
            rbuff_io[2 * i + 1] = s;
        }
        for (; i < outsize; i++)
        {
            rbuff_io[2 * i]     = 0.0;
            rbuff_io[2 * i + 1] = 0.0;
        }
    }

    /* 6) Periodic RX sync/SNR (~once per second).  Short line.
     *    Level is the per-second peak |sample| of the decoder input
     *    (post VAC1 RXGain), expressed in dBFS where 1.0 = 0 dBFS.
     *    clip=1 when peak >= 0.8 (mirrors FreeDV-GUI's FROM_RADIO_MAX). */
    if (++g_rx_diag_counter >= 750)
    {
        char buf[160];
        int sync = (int)_InterlockedAnd(&g_radae_sync, 0xffffffff);
        int snr  = (int)_InterlockedAnd(&g_radae_snr_db, 0xffffffff);
        float peak = g_rx_in_peak;
        float lvl_db;
        int clip = (peak >= 0.8f) ? 1 : 0;
        if (peak < 1e-9f) peak = 1e-9f;
        lvl_db = 20.0f * (float)log10((double)peak);
        sprintf_s(buf, sizeof(buf),
            "[RADAE] rx sync=%d snr=%d dB lvl=%.1f dBFS clip=%d\n",
            sync, snr, lvl_db, clip);
        OutputDebugStringA(buf);
        g_rx_diag_counter = 0;
        g_rx_in_peak = 0.0f;
    }

    LeaveCriticalSection(&g_radae_cs);
}

/* ============================================================
 * Hot-path: TX
 *  Encoder input  : r8brain mic 48k -> 16k speech
 *  Encoder output : r8brain modem 8k -> 48k -> rmatchV 48k -> 48k -> mic_io
 * ============================================================ */

#define RADE_TX_SCALE  0.5f

void xradae_tx(double* mic_io)
{
    const int tx_in_id = inid(1, 0);
    const int outrate  = pcm ? pcm->xcm_inrate[tx_in_id]  : -1;
    const int outsize  = pcm ? pcm->xcm_insize[tx_in_id]  : -1;

    long en = _InterlockedAnd(&g_radae_tx_enabled, 1);
    if (!en) return;
    if (!g_initialized || g_rade == NULL) return;
    if (outrate <= 0 || outsize <= 0) return;

    /* Test G -- coarsest bypass.  Read this flag BEFORE entering the
     * critical section so the early-return is the cheapest possible
     * no-op (no lock, no mic_io read, no mic_io write).  Behaves
     * exactly as if chkRADAE were off as far as xradae_tx is
     * concerned, but every other chkRADAE side-effect (mode forced,
     * VAC1 disabled, VACPreamp=1.0, xradae_rx running, RADE Reporter
     * polling) stays active.  If bumps disappear here, the cause is
     * something xradae_tx itself touches; if they remain, the cause
     * is in those side-effects (most likely a mode-driven TXA stage). */
    if (_InterlockedAnd(&g_radae_bypass_all, 1)) return;

    /* TX-side lock only -- xradae_rx and the UI use g_radae_cs which
     * is held briefly when needed (e.g. around rade_text access in
     * step 4 EOO).  See the comment block on g_radae_tx_cs near the
     * top of this file for the rationale: xradae_rx runs CPU-heavy
     * neural-net calls (rade_rx + FARGAN synthesis) inside its lock
     * hold; sharing that lock with xradae_tx made TX-block processing
     * wait past its 1.33 ms deadline -> audio engine glitch -> broadband
     * skirt bumps on the RF output.  Splitting the locks eliminates
     * the cross-thread serialisation. */
    EnterCriticalSection(&g_radae_tx_cs);

    /* MOX RX->TX edge: there is intentionally nothing to reset here.
     *
     * xradae_tx runs on EVERY audio block whenever chkRADAE is on, not
     * just during TX -- the early-return in this function is gated by
     * g_radae_tx_enabled (set by SetRadaeTxEnabled, only on chkRADAE
     * toggle), NOT by MOX state.  So the entire encoder pipeline (mic
     * DSP -> r8brain down -> speech FIFO -> LPCNet -> rade_tx -> modem
     * FIFO -> r8brain up -> outrate FIFO -> rmatchV -> mic_io -> dexp
     * audring -> fexchange0) is running continuously during both RX
     * and TX modes, producing a continuous OFDM stream that the radio's
     * MOX hardware gates on/off for actual emission.
     *
     * Resetting any of that state at MOX 0->1 destroys in-flight modem
     * samples and creates a ~120 ms silence trough plus two step
     * transitions (clean modem -> zero at ~42 ms; zero -> clean modem at
     * ~162 ms) that surface as broadband skirt splatter and receiver
     * sync loss.  freedv-gui resets its pipeline at MOX 0->1 because its
     * pipeline only runs during TX -- that pattern does not apply here.
     *
     * Continuous pipeline + continuous OFDM stream is what the protocol
     * actually needs.  RADE V1 OFDM uses continuous pilot symbols for
     * sync tracking; receivers can lock onto any clean segment of the
     * stream, so there is no "first preamble" that needs special
     * handling at MOX 0->1.
     *
     * Keep the flag consumption (so it doesn't latch) and the per-over
     * rmatchV diagnostic-counter reset (useful for telemetry; touches
     * only counters, not the ring state).  Drop everything else. */
    if (_InterlockedExchange(&g_radae_box_pending, 0))
    {
        if (g_tx_rmatch != NULL) resetRMatchDiags(g_tx_rmatch);
    }

    /* (Re)build the resamplers if outrate or outsize changed. */
    if (g_tx_resamp_down == NULL || g_tx_resamp_up == NULL || g_tx_rmatch == NULL ||
        outrate != g_tx_outrate_cached || outsize != g_tx_outsize_cached)
    {
        rebuild_tx_resamplers(outrate, outsize);
    }

    /* DIAGNOSTIC -- atomic snapshot of the four bypass flags for this
     * audio block.  Each flag short-circuits one stage in the encoder
     * pipeline so we can isolate which stage is producing the residual
     * skirt splatter / receiver-decoder clicking.  See the comment
     * block on the g_radae_bypass_* globals near the top of this file
     * for the full description of what each flag bypasses. */
    const int bypass_micdsp  = (int)_InterlockedAnd(&g_radae_bypass_micdsp,  1);
    const int bypass_core    = (int)_InterlockedAnd(&g_radae_bypass_core,    1);
    const int bypass_rmatch  = (int)_InterlockedAnd(&g_radae_bypass_rmatch,  1);
    const int bypass_encoder = (int)_InterlockedAnd(&g_radae_bypass_encoder, 1);

    /* 1) deswizzle L into mono float, apply g_radae_mic_scale (driven
     *    by the "RADE Mic level" spinner in Setup -> Audio -> Options;
     *    independent of VAC1 TXGain which scales the encoder OUTPUT
     *    via xpanel).  Also publish per-block peak as dBFS + clip flag
     *    for the TX mic meters -- this is the natural "Frm Mic" scope
     *    tap. */
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

    /* 1b) Pre-encoder mic-conditioning DSP chain (RNNoise + AGC +
     *     3-band biquad EQ).  All stages off by default; each stage's
     *     enable is independent and the chain is in-place on
     *     g_tx_in_mono.  Mirrors FreeDV-GUI 2.3.0's mic pipeline.
     *
     *     May return fewer samples than outsize during startup priming
     *     (RNNoise's first 480-sample frame is discarded; AGC accumulates
     *     a 10 ms block before producing output).  Propagate the actual
     *     count to the encoder-input resampler so we do NOT feed zero
     *     samples downstream -- zero samples in the first frame are
     *     encoded as silence by LPCNet and surface as a "swallowed first
     *     syllable" on receivers.  The downstream FIFOs and r8brain are
     *     all variable-rate by design so the smaller count is fine.
     *
     *     DIAGNOSTIC bypass_micdsp: skip micdsp; n_dsp = outsize so the
     *     raw post-Stage-1 mono mic passes through unchanged.
     *
     * 2) ENCODER INPUT RESAMPLER: r8brain outrate -> 16 kHz, push to
     *    speech FIFO.  Skip when the DSP chain emitted nothing this
     *    block (startup priming).
     *
     *    DIAGNOSTIC bypass_encoder: skip Stages 2..7 entirely.  Write
     *    the (optionally micdsp'd) mic_in_mono directly to mic_io as
     *    L=R 48 kHz doubles and return.  Sends raw SSB voice over the
     *    air (NOT a RADE signal) -- the receiver will hear voice, not
     *    decode.  Single decisive bisect of whether the bumps cause
     *    is inside Stages 2..7. */
    {
        int n_dsp = bypass_micdsp ? outsize
                                  : radae_micdsp_process(g_tx_in_mono, outsize);
        if (bypass_encoder)
        {
            int i;
            for (i = 0; i < n_dsp; i++)
            {
                /* I = real audio, Q = 0.  Matches the convention the
                 * HPSDR Protocol-2 mic handler uses (network.c:769) and
                 * that WDSP TXA's SSB modulator expects -- the internal
                 * Hilbert filter generates the analytic signal from
                 * Q=0 input.  Writing the real audio into BOTH I and Q
                 * makes the modulator see an "already-analytic" signal
                 * with I=Q, which produces spurious in-band products
                 * (skirt bumps on the panadapter, audible click /
                 * unintelligible FARGAN synthesis on the receiver). */
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
        if (n_dsp > 0)
        {
            int n16 = r8b_process_ff(g_tx_resamp_down, g_tx_in_mono, n_dsp,
                                     g_tx_speech_16k, RADAE_MAX_RESAMP_OUT);
            if (n16 > 0)
                fifo_push_check(g_tx_speech_fifo, &g_tx_speech_fifo_n, g_tx_speech_fifo_cap,
                                g_tx_speech_16k, n16,
                                &g_tx_speech_ovrun_count, "tx.speech_fifo");
        }
    }

    /* 3) drain speech FIFO in LPCNET_FRAME_SIZE chunks, accumulate features,
     *    invoke rade_tx, push 8 kHz modem audio to modem FIFO. */
    while (g_tx_speech_fifo_n >= LPCNET_FRAME_SIZE)
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
                    int n_modem = rade_tx(g_rade, g_rade_tx_out, g_tx_features_buf);
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
                    /* DIAGNOSTIC bypass_core: push g_rade_n_tx_out
                     * samples of silence into the modem FIFO instead of
                     * calling rade_tx().  Downstream Stages 6 and 7
                     * still run on this silent input -- if the skirt
                     * bumps disappear in this mode, the OFDM output
                     * of rade_tx() is the source. */
                    int silence_n = g_rade_n_tx_out;
                    memset(g_tx_modem_8k, 0, (size_t)silence_n * sizeof(float));
                    fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                                    g_tx_modem_8k, silence_n,
                                    &g_tx_modem_ovrun_count, "tx.modem_fifo");
                }
            }
        }
    }

    /* 4) EOO handling: emit one EOO frame + 60 ms silence on TX -> RX flip.
     *    Just before calling rade_tx_eoo, encode our cached callsign
     *    via FreeDV-GUI's rade_text codec (LDPC(112,56) + CRC8 + 6-bit
     *    charset + Gray interleaver) and inject it into the EOO bits
     *    via rade_tx_set_eoo_bits.  This is what FreeDV-GUI 2.3.0
     *    receivers expect on-air. */
    if (_InterlockedExchange(&g_radae_eoo_pending, 0))
    {
        if (g_rade_text != NULL && g_tx_own_callsign[0] != '\0' &&
            g_rade_eoo_bits != NULL && g_rade_n_eoo_bits > 0)
        {
            /* rade_text_generate_tx_string takes a symSize arg expressed
             * as (I,Q) pairs of floats -- divide bit count by 2.
             *
             * rade_text codec is shared with xradae_rx's rade_text_rx
             * callback path -- briefly grab g_radae_cs here so the two
             * threads don't race on the codec's internal state.  This
             * is a rare event (only fires on MOX 1->0 edge) so the
             * inter-thread serialisation cost is negligible. */
            if (g_radae_cs_inited) EnterCriticalSection(&g_radae_cs);
            rade_text_generate_tx_string(g_rade_text,
                                         g_tx_own_callsign,
                                         (int)strlen(g_tx_own_callsign),
                                         g_rade_eoo_bits,
                                         g_rade_n_eoo_bits / 2);
            rade_tx_set_eoo_bits(g_rade, g_rade_eoo_bits);
            if (g_radae_cs_inited) LeaveCriticalSection(&g_radae_cs);
        }
        int n_eoo = rade_tx_eoo(g_rade, g_rade_tx_eoo_out);
        int j;
        if (n_eoo > 0)
        {
            for (j = 0; j < n_eoo; j++)
                g_tx_modem_8k[j] = g_rade_tx_eoo_out[j].real * RADE_TX_SCALE;
            fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                            g_tx_modem_8k, n_eoo,
                            &g_tx_modem_ovrun_count, "tx.modem_fifo");
        }
        {
            float zeros[480];
            memset(zeros, 0, sizeof(zeros));
            fifo_push_check(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_fifo_cap,
                            zeros, 480,
                            &g_tx_modem_ovrun_count, "tx.modem_fifo");
        }
    }

    /* 5) ENCODER OUTPUT RESAMPLER: r8brain 8 kHz modem -> outrate, push to outrate FIFO */
    {
        const int MODEM_POP_CAP = 1024;
        const int OUT_CAP       = RADAE_MAX_RESAMP_OUT;
        while (g_tx_modem_fifo_n > 0)
        {
            int take = g_tx_modem_fifo_n;
            int nout = 0;
            if (take > MODEM_POP_CAP) take = MODEM_POP_CAP;
            fifo_pop(g_tx_modem_fifo, &g_tx_modem_fifo_n, g_tx_modem_8k, take);
            nout = r8b_process_ff(g_tx_resamp_up, g_tx_modem_8k, take,
                                  g_tx_modem_outrate, OUT_CAP);
            if (nout > 0)
                fifo_push_check(g_tx_outrate_fifo, &g_tx_outrate_fifo_n, g_tx_outrate_fifo_cap,
                                g_tx_modem_outrate, nout,
                                &g_tx_outrate_ovrun_count, "tx.outrate_fifo");
        }
    }

    /* 6) RMATCH-V LAYER: every block, push exactly rmatch_blksz samples
     *    to rmatchV via xrmatchIN; then xrmatchOUT directly into mic_io.
     *
     *    If outrate FIFO doesn't have a full block ready (the typical
     *    case during the ~120 ms preamble gap at the start of every
     *    over while LPCNet accumulates 12 feature frames before the
     *    first rade_tx() fires), push ZEROS instead of skipping the
     *    xrmatchIN.  This keeps rmatchV's ring at steady fill so
     *    xrmatchOUT does not trigger dslew (a 3 ms Hann fade-to-zero
     *    + zero-fill of the rest of the block) on every block.  The
     *    dslew transients, repeated at the audio-block rate (~750 Hz
     *    at outsize=64), are what produces the broadband splatter on
     *    the SSB output at the start of each over.  Spectrally clean
     *    silence is far better than a comb of Hann-tapered transients.
     *    Diag counter still increments so the rate-limited log line
     *    reflects how often the zero-feed kicks in. */
    if (g_tx_rmatch != NULL && g_tx_rmatch_blksz > 0 &&
        g_tx_rmatch_blksz <= outsize)   /* must match how we configured rmatchV */
    {
        const int blk = g_tx_rmatch_blksz;
        int i;

        /* DIAGNOSTIC bypass_rmatch: skip xrmatchIN/xrmatchOUT entirely;
         * copy the outrate FIFO directly into mic_io.  If the skirt
         * bumps disappear here, rmatchV's varsamp linear interpolation
         * is the source.  Pad with silence when the FIFO is short
         * (same zero-feed semantics as fix A -- spectrally clean). */
        if (bypass_rmatch)
        {
            float scratch[RADAE_MAX_BLOCK];
            int have = (g_tx_outrate_fifo_n < blk) ? g_tx_outrate_fifo_n : blk;
            if (have > 0)
                fifo_pop(g_tx_outrate_fifo, &g_tx_outrate_fifo_n, scratch, have);
            for (i = 0; i < have; i++)
            {
                /* I = modem real, Q = 0 (see comment above the bypass_encoder
                 * branch).  Writing modem to both slots makes WDSP SSB
                 * generate spurious products. */
                mic_io[2 * i]     = (double)scratch[i];
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

        if (g_tx_outrate_fifo_n >= blk)
        {
            float scratch[RADAE_MAX_BLOCK];
            fifo_pop(g_tx_outrate_fifo, &g_tx_outrate_fifo_n, scratch, blk);
            for (i = 0; i < blk; i++)
            {
                double v = (double)scratch[i];
                g_tx_rmatch_in[2 * i]     = v;
                g_tx_rmatch_in[2 * i + 1] = v;
            }
        }
        else
        {
            long c;
            memset(g_tx_rmatch_in, 0, (size_t)(2 * blk) * sizeof(double));
            c = ++g_tx_outrate_underrun_count;
            if (c == 1 || (c % 50) == 0)
            {
                char log[140];
                sprintf_s(log, sizeof(log),
                    "[RADAE] outrate->rmatch UNDR have=%d need=%d total=%ld (zero-fed)\n",
                    g_tx_outrate_fifo_n, blk, c);
                OutputDebugStringA(log);
            }
        }
        xrmatchIN(g_tx_rmatch, g_tx_rmatch_in);
        /* xrmatchOUT writes 2*blk doubles into our scratch.  blk == outsize
         * by configuration. */
        xrmatchOUT(g_tx_rmatch, g_tx_rmatch_out);

        /* LOOPBACK: divert L of rmatchV output into the bridge ring; mic_io
         * stays silent so the radio does not transmit while looping. */
        if (_InterlockedAnd(&g_radae_loopback_enabled, 0xffffffff))
        {
            float bridge_scratch[RADAE_MAX_BLOCK];
            int avail, take;
            for (i = 0; i < blk; i++)
                bridge_scratch[i] = (float)g_tx_rmatch_out[2 * i];
            avail = RADAE_LOOP_BRIDGE_CAP - g_loop_bridge_n;
            take  = (blk < avail) ? blk : avail;
            if (take > 0)
            {
                memcpy(g_loop_bridge + g_loop_bridge_n, bridge_scratch,
                       (size_t)take * sizeof(float));
                g_loop_bridge_n += take;
            }
            if (take < blk)
            {
                long c = ++g_loop_bridge_ovrun_count;
                if (c == 1 || (c % 50) == 0)
                {
                    char log[120];
                    sprintf_s(log, sizeof(log),
                        "[RADAE] loop_bridge OVRUN dropped=%d total=%ld\n",
                        blk - take, c);
                    OutputDebugStringA(log);
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
            for (i = 0; i < blk; i++)
            {
                /* I = real modem audio, Q = 0.  Matches the network
                 * mic handler's (real, 0) convention -- WDSP TXA's SSB
                 * modulator generates the analytic signal internally via
                 * its Hilbert filter and expects Q=0.  Writing modem on
                 * both slots creates spurious in-band products that
                 * appear as skirt bumps on the panadapter and audible
                 * clicks on the receiver decoder.  rmatchV processed
                 * both slots above (L=R=modem) but only L is used here. */
                mic_io[2 * i]     = g_tx_rmatch_out[2 * i];
                mic_io[2 * i + 1] = 0.0;
            }
        }
    }
    else
    {
        /* rmatch not yet built (very first call): fill mic_io with silence. */
        int i;
        for (i = 0; i < outsize; i++)
        {
            mic_io[2 * i]     = 0.0;
            mic_io[2 * i + 1] = 0.0;
        }
    }

    LeaveCriticalSection(&g_radae_tx_cs);
}
