/*  radae_micdsp.c
 *
 *  Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
 *  RNNoise upstream: Xiph.Org Foundation / Mozilla / Amazon (BSD-3).
 *  libebur128 upstream: jiixyj (MIT).
 *  WebRTC_AGC upstream: WebRTC project (BSD-3).
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <Windows.h>

#include "radae_micdsp.h"
#include "rnnoise.h"
#include "ebur128.h"
#include "agc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================
 * Biquad helpers (Direct-Form-II Transposed).  Coefficients are
 * the standard RBJ Audio EQ Cookbook formulas, normalised so
 * a0 == 1.
 * ============================================================ */

typedef struct
{
    double b0, b1, b2;     /* normalised numerator coefficients */
    double a1, a2;         /* normalised denominator coefficients */
    double z1, z2;         /* DF-II-transposed state */
    int    enabled;        /* 0 -> bypass (output = input) */
} biquad_t;

static void biquad_reset(biquad_t* b)
{
    b->z1 = b->z2 = 0.0;
}

static void biquad_clear(biquad_t* b)
{
    b->b0 = 1.0; b->b1 = 0.0; b->b2 = 0.0;
    b->a1 = 0.0; b->a2 = 0.0;
    b->z1 = b->z2 = 0.0;
    b->enabled = 0;
}

/* Process one sample through a Direct-Form-II Transposed biquad.
 * The bypass branch is sample-cheap and lets the chain be wired
 * unconditionally without a per-sample if. */
static __forceinline float biquad_process(biquad_t* b, float in)
{
    if (!b->enabled) return in;
    double x  = (double)in;
    double y  = b->b0 * x + b->z1;
    b->z1     = b->b1 * x - b->a1 * y + b->z2;
    b->z2     = b->b2 * x - b->a2 * y;
    return (float)y;
}

/* ----- Coefficient designers (RBJ cookbook) ----- */

/* peakingEQ at f0 / Q with gain dB.  Used for the mid band. */
static void biquad_design_peaking(biquad_t* b, double sr, double f0, double gain_db, double q)
{
    if (sr <= 0.0 || f0 <= 0.0 || q <= 0.0) { biquad_clear(b); return; }
    double A     = pow(10.0, gain_db / 40.0);
    double w0    = 2.0 * M_PI * f0 / sr;
    double cosw0 = cos(w0);
    double alpha = sin(w0) / (2.0 * q);

    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cosw0;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha / A;

    b->b0 = b0 / a0; b->b1 = b1 / a0; b->b2 = b2 / a0;
    b->a1 = a1 / a0; b->a2 = a2 / a0;
}

/* lowshelf at f0 with gain dB, slope S=1.0 (matches SoX `bass`). */
static void biquad_design_lowshelf(biquad_t* b, double sr, double f0, double gain_db)
{
    if (sr <= 0.0 || f0 <= 0.0) { biquad_clear(b); return; }
    double A     = pow(10.0, gain_db / 40.0);
    double w0    = 2.0 * M_PI * f0 / sr;
    double cosw0 = cos(w0);
    double sinw0 = sin(w0);
    double S     = 1.0;
    double alpha = sinw0 / 2.0 * sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
    double sqrtAa = 2.0 * sqrt(A) * alpha;

    double b0 =    A * ( (A + 1.0) - (A - 1.0) * cosw0 + sqrtAa );
    double b1 =  2*A * ( (A - 1.0) - (A + 1.0) * cosw0 );
    double b2 =    A * ( (A + 1.0) - (A - 1.0) * cosw0 - sqrtAa );
    double a0 =          (A + 1.0) + (A - 1.0) * cosw0 + sqrtAa;
    double a1 =   -2.0 * ( (A - 1.0) + (A + 1.0) * cosw0 );
    double a2 =          (A + 1.0) + (A - 1.0) * cosw0 - sqrtAa;

    b->b0 = b0 / a0; b->b1 = b1 / a0; b->b2 = b2 / a0;
    b->a1 = a1 / a0; b->a2 = a2 / a0;
}

/* highshelf at f0 with gain dB, slope S=1.0 (matches SoX `treble`). */
static void biquad_design_highshelf(biquad_t* b, double sr, double f0, double gain_db)
{
    if (sr <= 0.0 || f0 <= 0.0) { biquad_clear(b); return; }
    double A     = pow(10.0, gain_db / 40.0);
    double w0    = 2.0 * M_PI * f0 / sr;
    double cosw0 = cos(w0);
    double sinw0 = sin(w0);
    double S     = 1.0;
    double alpha = sinw0 / 2.0 * sqrt((A + 1.0/A) * (1.0/S - 1.0) + 2.0);
    double sqrtAa = 2.0 * sqrt(A) * alpha;

    double b0 =    A * ( (A + 1.0) + (A - 1.0) * cosw0 + sqrtAa );
    double b1 = -2*A * ( (A - 1.0) + (A + 1.0) * cosw0 );
    double b2 =    A * ( (A + 1.0) + (A - 1.0) * cosw0 - sqrtAa );
    double a0 =          (A + 1.0) - (A - 1.0) * cosw0 + sqrtAa;
    double a1 =    2.0 * ( (A - 1.0) - (A + 1.0) * cosw0 );
    double a2 =          (A + 1.0) - (A - 1.0) * cosw0 - sqrtAa;

    b->b0 = b0 / a0; b->b1 = b1 / a0; b->b2 = b2 / a0;
    b->a1 = a1 / a0; b->a2 = a2 / a0;
}

/* Linear gain biquad (b0 = gain, all other = 0).  Trivially stable;
 * Direct-Form path through biquad_process produces y = b0 * x. */
static void biquad_design_gain(biquad_t* b, double gain_db)
{
    double g = pow(10.0, gain_db / 20.0);
    b->b0 = g; b->b1 = 0.0; b->b2 = 0.0;
    b->a1 = 0.0; b->a2 = 0.0;
}

/* ============================================================
 * Module state.
 * ============================================================ */

#define MICDSP_RNNOISE_RATE       48000
#define MICDSP_RNNOISE_FRAME_SIZE 480     /* 10 ms @ 48 kHz */
#define MICDSP_FIFO_CAP           4096    /* >= 1 RNNoise frame + headroom */

static int           g_sr                 = 0;
static int           g_ready              = 0;

/* ----- RNNoise state ----- */
static volatile long g_rnnoise_enable     = 0;
static DenoiseState* g_rnnoise            = NULL;
static int           g_first_rnnoise_frame= 1;
static float         g_rnn_in_fifo[MICDSP_FIFO_CAP];
static int           g_rnn_in_fifo_n      = 0;
static float         g_rnn_out_fifo[MICDSP_FIFO_CAP];
static int           g_rnn_out_fifo_n     = 0;

/* ----- AGC state (mirrors freedv-gui AgcStep.cpp) ----- */
static volatile long g_agc_enable         = 0;
static volatile double g_agc_target_lufs  = -23.0;

#define MICDSP_AGC_MAX_DB        12.0     /* AGC_MAX_GAIN_DB         */
#define MICDSP_AGC_MIN_DB       -20.0     /* AGC_MIN_GAIN_DB         */
#define MICDSP_AGC_ATTACK_S      0.5      /* AGC_ATTACK_TIME_SEC     */
#define MICDSP_AGC_RELEASE_S     6.0      /* AGC_RELEASE_TIME_SEC    */
#define MICDSP_AGC_SILENCE_LUFS -33.0     /* SILENCE_THRESHOLD_LUFS  */
#define MICDSP_AGC_LIMITER_DB    1        /* -LIMITER_LEVEL_DB (= -(-1)) */
#define MICDSP_AGC_TEN_MS_DIVIDER 100     /* AgcStep TEN_MS_DIVIDER  */
#define MICDSP_AGC_MAX_SAMPLES    160     /* AgcStep MAX_AGC_SAMPLES */
#define MICDSP_AGC_FIFO_CAP       (48000) /* 1 s mono @ 48 kHz       */

static ebur128_state* g_ebur128           = NULL;
static void*          g_webrtc_agc        = NULL;     /* opaque agc inst */
static int            g_agc_samples_per_run = 0;
static double         g_agc_current_db    = 0.0;
static double         g_agc_target_db     = 0.0;

/* AGC bridging FIFOs: float blocks in -> short blocks chunked into
 * AgcStep's numSamplesPerRun_ runs -> short blocks back to float out.
 * Mirrors AgcStep's inputSampleFifo_ + outputSamples_ shape. */
static int16_t        g_agc_in_fifo[MICDSP_AGC_FIFO_CAP];
static int            g_agc_in_fifo_n     = 0;
static int16_t        g_agc_out_fifo[MICDSP_AGC_FIFO_CAP];
static int            g_agc_out_fifo_n    = 0;

/* ----- EQ state ----- */
static volatile long g_eq_enable          = 0;
static biquad_t      g_eq_bass;
static biquad_t      g_eq_mid;
static biquad_t      g_eq_treble;
static biquad_t      g_eq_vol;
static volatile long g_eq_dirty           = 0;

/* Latched EQ params (UI thread writes; audio thread reads on a
 * dirty-flag transition). */
static volatile double g_eq_bass_freq     = 100.0;
static volatile double g_eq_bass_gain     =   0.0;
static volatile double g_eq_mid_freq      = 1000.0;
static volatile double g_eq_mid_gain      =   0.0;
static volatile double g_eq_mid_q         =   0.707;
static volatile double g_eq_treble_freq   = 5000.0;
static volatile double g_eq_treble_gain   =   0.0;
static volatile double g_eq_vol_gain_db   =   0.0;

/* ============================================================
 * Lifecycle
 * ============================================================ */

static int agc_supported_rate(int sr)
{
    return (sr == 8000 || sr == 16000 || sr == 32000 || sr == 48000);
}

static void agc_init_engines(void)
{
    /* AgcStep: numSamplesPerRun_ = min(MAX_AGC_SAMPLES, sr/100) */
    int n_per_run = g_sr / MICDSP_AGC_TEN_MS_DIVIDER;
    if (n_per_run > MICDSP_AGC_MAX_SAMPLES) n_per_run = MICDSP_AGC_MAX_SAMPLES;
    if (n_per_run < 1) n_per_run = 1;
    g_agc_samples_per_run = n_per_run;

    /* libebur128 -- EBUR128_MODE_S gives both shortterm and momentary;
     * AgcStep queries momentary. */
    if (g_ebur128 != NULL) { ebur128_destroy(&g_ebur128); g_ebur128 = NULL; }
    if (agc_supported_rate(g_sr))
        g_ebur128 = ebur128_init(1, (unsigned long)g_sr, EBUR128_MODE_S);

    /* WebRTC AGC -- pure limiter (kAgcModeUnchanged, comp gain 0,
     * limiter enabled, target = -LIMITER_LEVEL_DB = 1 dBFS below clip). */
    if (g_webrtc_agc != NULL) { WebRtcAgc_Free(g_webrtc_agc); g_webrtc_agc = NULL; }
    if (agc_supported_rate(g_sr))
    {
        g_webrtc_agc = WebRtcAgc_Create();
        if (g_webrtc_agc != NULL)
        {
            int st = WebRtcAgc_Init(g_webrtc_agc, 0, 255, kAgcModeUnchanged, (uint32_t)g_sr);
            if (st == 0)
            {
                WebRtcAgcConfig cfg;
                cfg.compressionGaindB = 0;
                cfg.limiterEnable     = 1;
                cfg.targetLevelDbfs   = MICDSP_AGC_LIMITER_DB;
                if (WebRtcAgc_set_config(g_webrtc_agc, cfg) != 0)
                {
                    WebRtcAgc_Free(g_webrtc_agc);
                    g_webrtc_agc = NULL;
                }
            }
            else
            {
                WebRtcAgc_Free(g_webrtc_agc);
                g_webrtc_agc = NULL;
            }
        }
    }

    g_agc_current_db = 0.0;
    g_agc_target_db  = 0.0;
    g_agc_in_fifo_n  = 0;
    g_agc_out_fifo_n = 0;
}

/* Soft reset -- mirrors freedv-gui AgcStep::reset() exactly:
 * zero the smoothed-gain accumulators and clear the input FIFO, but
 * leave the existing ebur128_state and WebRtcAgc instances allocated
 * so their internal histories (loudness ring, limiter envelope)
 * survive the reset.  Our output FIFO is also cleared because it's
 * the bridging buffer between AGC blocks and the float-block caller
 * contract -- the freedv-gui equivalent (outputSamples_) is naturally
 * overwritten on every execute() call, so retaining stale samples
 * here would diverge from upstream. */
static void agc_soft_reset(void)
{
    g_agc_current_db = 0.0;
    g_agc_target_db  = 0.0;
    g_agc_in_fifo_n  = 0;
    g_agc_out_fifo_n = 0;
}

void radae_micdsp_create(int sample_rate)
{
    if (sample_rate <= 0) sample_rate = 48000;
    g_sr = sample_rate;

    /* RNNoise.  rnnoise_create() with NULL == default model. */
    g_rnnoise = rnnoise_create(NULL);
    g_first_rnnoise_frame = 1;
    g_rnn_in_fifo_n = 0;
    g_rnn_out_fifo_n = 0;

    /* AGC engines (libebur128 + WebRTC AGC). */
    agc_init_engines();

    /* EQ */
    biquad_clear(&g_eq_bass);
    biquad_clear(&g_eq_mid);
    biquad_clear(&g_eq_treble);
    biquad_clear(&g_eq_vol);
    g_eq_dirty = 1;   /* force first design pass on first process call */

    g_ready = 1;
    OutputDebugStringA("[RADAE-MICDSP] ready\n");
}

void radae_micdsp_destroy(void)
{
    g_ready = 0;
    if (g_rnnoise != NULL)    { rnnoise_destroy(g_rnnoise); g_rnnoise = NULL; }
    if (g_ebur128 != NULL)    { ebur128_destroy(&g_ebur128); g_ebur128 = NULL; }
    if (g_webrtc_agc != NULL) { WebRtcAgc_Free(g_webrtc_agc); g_webrtc_agc = NULL; }
}

void radae_micdsp_reset(void)
{
    if (!g_ready) return;

    /* RNNoise: clear bridging FIFOs and re-prime first-frame discard.
     * The neural net's running state is implicitly retained inside the
     * DenoiseState; freedv-gui's RNNoiseStep::reset() also keeps it. */
    g_rnn_in_fifo_n       = 0;
    g_rnn_out_fifo_n      = 0;
    g_first_rnnoise_frame = 1;

    /* AGC: zero the smoothed-gain accumulators and clear the bridging
     * FIFOs, but leave ebur128 + WebRTC instances allocated -- their
     * internal histories survive the reset.  Exact parity with
     * freedv-gui's AgcStep::reset(). */
    agc_soft_reset();

    /* EQ biquad z-state -- bass / mid / treble / vol. */
    biquad_reset(&g_eq_bass);
    biquad_reset(&g_eq_mid);
    biquad_reset(&g_eq_treble);
    biquad_reset(&g_eq_vol);
}

/* ============================================================
 * Setters
 * ============================================================ */

void radae_micdsp_set_rnnoise_enabled(int e)
{
    long was = _InterlockedExchange(&g_rnnoise_enable, e ? 1 : 0);
    if (e && !was) g_first_rnnoise_frame = 1;   /* re-prime on enable */
}

void radae_micdsp_set_agc_enabled(int e)
{
    long was = _InterlockedExchange(&g_agc_enable, e ? 1 : 0);
    if (e && !was)
    {
        /* On disabled -> enabled edge: zero the gain accumulators and
         * clear the bridging FIFOs (matches AgcStep::reset()).  The
         * ebur128 + WebRTC instances stay allocated. */
        agc_soft_reset();
    }
}

void radae_micdsp_set_agc_target_lufs(double t)
{
    if (t < -40.0) t = -40.0;
    if (t > 0.0)   t = 0.0;
    g_agc_target_lufs = t;
}

void radae_micdsp_set_eq_enabled(int e)
{
    _InterlockedExchange(&g_eq_enable, e ? 1 : 0);
    g_eq_dirty = 1;
}

void radae_micdsp_set_eq_bass(double f, double g)
{
    g_eq_bass_freq = f;
    g_eq_bass_gain = g;
    g_eq_dirty = 1;
}

void radae_micdsp_set_eq_mid(double f, double g, double q)
{
    g_eq_mid_freq = f;
    g_eq_mid_gain = g;
    g_eq_mid_q    = q;
    g_eq_dirty = 1;
}

void radae_micdsp_set_eq_treble(double f, double g)
{
    g_eq_treble_freq = f;
    g_eq_treble_gain = g;
    g_eq_dirty = 1;
}

void radae_micdsp_set_eq_vol(double db)
{
    g_eq_vol_gain_db = db;
    g_eq_dirty = 1;
}

/* ============================================================
 * EQ rebuild (audio-thread).  Runs at most once per UI change.
 * ============================================================ */

static void eq_rebuild_if_dirty(void)
{
    if (!_InterlockedExchange(&g_eq_dirty, 0)) return;
    /* Read all volatile params under no critical section -- they are
     * doubles so torn reads are theoretically possible on x86 (8-byte
     * reads are not atomic on 32-bit MOV pairs).  Practically the
     * compiler emits MOVSD on x64 which is atomic; on 32-bit this
     * could see torn data.  Worst case: one block of slightly wrong
     * EQ; resync next frame. */
    double bf = g_eq_bass_freq,   bg = g_eq_bass_gain;
    double mf = g_eq_mid_freq,    mg = g_eq_mid_gain,    mq = g_eq_mid_q;
    double tf = g_eq_treble_freq, tg = g_eq_treble_gain;
    double vg = g_eq_vol_gain_db;
    int    eq_on = (int)_InterlockedAnd(&g_eq_enable, 1);

    if (eq_on && fabs(bg) > 1e-6) {
        biquad_design_lowshelf(&g_eq_bass, (double)g_sr, bf, bg);
        g_eq_bass.enabled = 1;
    } else {
        g_eq_bass.enabled = 0;
    }

    if (eq_on && fabs(mg) > 1e-6) {
        biquad_design_peaking(&g_eq_mid, (double)g_sr, mf, mg, mq);
        g_eq_mid.enabled = 1;
    } else {
        g_eq_mid.enabled = 0;
    }

    if (eq_on && fabs(tg) > 1e-6) {
        biquad_design_highshelf(&g_eq_treble, (double)g_sr, tf, tg);
        g_eq_treble.enabled = 1;
    } else {
        g_eq_treble.enabled = 0;
    }

    /* Vol always available regardless of EQ master enable. */
    if (fabs(vg) > 1e-6) {
        biquad_design_gain(&g_eq_vol, vg);
        g_eq_vol.enabled = 1;
    } else {
        g_eq_vol.enabled = 0;
    }
}

/* ============================================================
 * AGC: identical call sequence to freedv-gui's AgcStep::execute().
 * Float in -> short[] in 10 ms chunks -> ebur128 momentary LUFS ->
 * smoothed gain -> WebRTC limiter -> short[] out -> float out.
 * ============================================================ */

static __forceinline int16_t float_to_s16(float f)
{
    float s = f * 32768.0f;
    if (s >  32767.0f) s =  32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (int16_t)(s >= 0.0f ? (s + 0.5f) : (s - 0.5f));
}

static __forceinline float s16_to_float(int16_t v)
{
    return (float)v / 32768.0f;
}

static int agc_run_one_block(int16_t* in_block, int16_t* out_block, int n)
{
    /* Step 1: feed samples into ebur128 and read momentary loudness. */
    double lufs = -HUGE_VAL;
    int result = -1;
    if (g_ebur128 != NULL)
    {
        ebur128_add_frames_short(g_ebur128, in_block, (size_t)n);
        result = ebur128_loudness_momentary(g_ebur128, &lufs);
    }

    if (result == EBUR128_SUCCESS && lufs != -HUGE_VAL &&
        lufs > MICDSP_AGC_SILENCE_LUFS)
    {
        /* Step 2: target = userTarget - measured. */
        double t = g_agc_target_lufs - lufs;
        if (t >= MICDSP_AGC_MAX_DB) t = MICDSP_AGC_MAX_DB;
        if (t <= MICDSP_AGC_MIN_DB) t = MICDSP_AGC_MIN_DB;
        g_agc_target_db = t;

        /* Step 3: slew current toward target -- attack vs release. */
        double interval = (g_agc_target_db < g_agc_current_db)
                          ? MICDSP_AGC_ATTACK_S : MICDSP_AGC_RELEASE_S;
        if (interval > 1e-9)
            g_agc_current_db += ((g_agc_target_db - g_agc_current_db) / interval)
                                * ((double)n / (double)g_sr);
    }

    /* Scale samples by current gain (in-place on the int16 block). */
    {
        double scale = pow(10.0, g_agc_current_db / 20.0);
        int i;
        for (i = 0; i < n; i++)
        {
            double s = (double)in_block[i] * scale;
            if (s >  32767.0) s =  32767.0;
            if (s < -32768.0) s = -32768.0;
            in_block[i] = (int16_t)s;
        }
    }

    /* Step 4: WebRTC limiter on the post-gain block. */
    if (g_webrtc_agc != NULL)
    {
        int32_t outMicLevel = 0;
        int32_t inMicLevel  = 0;
        int16_t echo = 0;
        uint8_t saturationWarning = 1;
        const int16_t* const in_arr[1]  = { in_block };
        int16_t* const out_arr[1]       = { out_block };
        WebRtcAgc_Process(g_webrtc_agc,
                          in_arr, 1, (size_t)n,
                          out_arr, inMicLevel, &outMicLevel,
                          echo, &saturationWarning);
    }
    else
    {
        memcpy(out_block, in_block, (size_t)n * sizeof(int16_t));
    }
    return n;
}

/* Returns the number of float samples actually written to buf.  May be
 * less than n during startup priming (AGC drains in 10 ms blocks, so
 * up to ~10 ms of input is buffered before any output appears).
 * Matches freedv-gui AgcStep::execute() semantics: the caller absorbs
 * the variable rate.  Do NOT zero-pad on under-fill -- those zeros
 * would otherwise become the first LPCNet frame's input on every MOX
 * RX->TX edge and surface as a "swallowed first syllable" on receivers. */
static int agc_step_block(float* buf, int n)
{
    if (g_ebur128 == NULL || g_webrtc_agc == NULL || g_agc_samples_per_run <= 0)
        return n;   /* AGC unsupported -- bypass: leave buf unchanged, report full count. */

    int n_per_run = g_agc_samples_per_run;

    /* Push float input -> short FIFO. */
    {
        int push = n;
        if (g_agc_in_fifo_n + push > MICDSP_AGC_FIFO_CAP)
            push = MICDSP_AGC_FIFO_CAP - g_agc_in_fifo_n;
        if (push > 0)
        {
            int i;
            for (i = 0; i < push; i++)
                g_agc_in_fifo[g_agc_in_fifo_n + i] = float_to_s16(buf[i]);
            g_agc_in_fifo_n += push;
        }
    }

    /* Drain in n_per_run-sample (= 10 ms) blocks. */
    while (g_agc_in_fifo_n >= n_per_run)
    {
        int16_t in_block[MICDSP_AGC_MAX_SAMPLES];
        int16_t out_block[MICDSP_AGC_MAX_SAMPLES];
        memcpy(in_block, g_agc_in_fifo, (size_t)n_per_run * sizeof(int16_t));
        memmove(g_agc_in_fifo, g_agc_in_fifo + n_per_run,
                (size_t)(g_agc_in_fifo_n - n_per_run) * sizeof(int16_t));
        g_agc_in_fifo_n -= n_per_run;

        agc_run_one_block(in_block, out_block, n_per_run);

        if (g_agc_out_fifo_n + n_per_run <= MICDSP_AGC_FIFO_CAP)
        {
            memcpy(g_agc_out_fifo + g_agc_out_fifo_n, out_block,
                   (size_t)n_per_run * sizeof(int16_t));
            g_agc_out_fifo_n += n_per_run;
        }
    }

    /* Pop short FIFO -> float output (write back to buf).  Return the
     * actual count produced; the caller propagates it downstream. */
    int avail = (g_agc_out_fifo_n < n) ? g_agc_out_fifo_n : n;
    int i;
    for (i = 0; i < avail; i++)
        buf[i] = s16_to_float(g_agc_out_fifo[i]);
    if (avail > 0)
    {
        memmove(g_agc_out_fifo, g_agc_out_fifo + avail,
                (size_t)(g_agc_out_fifo_n - avail) * sizeof(int16_t));
        g_agc_out_fifo_n -= avail;
    }
    return avail;
}

/* ============================================================
 * EQ block
 * ============================================================ */

static void eq_step_block(float* buf, int n)
{
    int i;
    int has_band = (g_eq_bass.enabled || g_eq_mid.enabled ||
                    g_eq_treble.enabled || g_eq_vol.enabled);
    if (!has_band) return;

    for (i = 0; i < n; i++)
    {
        float s = buf[i];
        s = biquad_process(&g_eq_bass,   s);
        s = biquad_process(&g_eq_mid,    s);
        s = biquad_process(&g_eq_treble, s);
        s = biquad_process(&g_eq_vol,    s);
        buf[i] = s;
    }
}

/* ============================================================
 * RNNoise block.  Operates in fixed 480-sample frames at 48 kHz.
 * Caller's sample rate must be 48 kHz for this to be enabled (we
 * gate it in the process function); arbitrary block sizes are
 * bridged via the FIFO.  In/out scaled to RNNoise's int16 range
 * internally.
 * ============================================================ */

static int rnnoise_step_block(float* buf, int n)
{
    if (n <= 0 || g_rnnoise == NULL) return 0;
    if (g_sr != MICDSP_RNNOISE_RATE) return 0;   /* 48 kHz only */

    /* Push input into FIFO. */
    if (g_rnn_in_fifo_n + n > MICDSP_FIFO_CAP)
        n = MICDSP_FIFO_CAP - g_rnn_in_fifo_n;
    if (n > 0)
    {
        /* RNNoise expects int16-scale floats; multiply by 32768. */
        int i;
        for (i = 0; i < n; i++)
            g_rnn_in_fifo[g_rnn_in_fifo_n + i] = buf[i] * 32768.0f;
        g_rnn_in_fifo_n += n;
    }

    /* Drain in 480-sample frames. */
    while (g_rnn_in_fifo_n >= MICDSP_RNNOISE_FRAME_SIZE)
    {
        float frame[MICDSP_RNNOISE_FRAME_SIZE];
        memcpy(frame, g_rnn_in_fifo, sizeof(frame));
        memmove(g_rnn_in_fifo, g_rnn_in_fifo + MICDSP_RNNOISE_FRAME_SIZE,
                (g_rnn_in_fifo_n - MICDSP_RNNOISE_FRAME_SIZE) * sizeof(float));
        g_rnn_in_fifo_n -= MICDSP_RNNOISE_FRAME_SIZE;

        rnnoise_process_frame(g_rnnoise, frame, frame);

        if (g_first_rnnoise_frame)
        {
            /* Discard first frame -- RNNoise needs one frame of warm-
             * up before its output is meaningful. */
            g_first_rnnoise_frame = 0;
            continue;
        }

        /* Push to output FIFO, scaled back to [-1, 1]. */
        if (g_rnn_out_fifo_n + MICDSP_RNNOISE_FRAME_SIZE <= MICDSP_FIFO_CAP)
        {
            int i;
            for (i = 0; i < MICDSP_RNNOISE_FRAME_SIZE; i++)
                g_rnn_out_fifo[g_rnn_out_fifo_n + i] = frame[i] / 32768.0f;
            g_rnn_out_fifo_n += MICDSP_RNNOISE_FRAME_SIZE;
        }
    }

    /* Pop available output samples back into buf. */
    int avail = (g_rnn_out_fifo_n < n) ? g_rnn_out_fifo_n : n;
    if (avail > 0)
    {
        memcpy(buf, g_rnn_out_fifo, avail * sizeof(float));
        memmove(g_rnn_out_fifo, g_rnn_out_fifo + avail,
                (g_rnn_out_fifo_n - avail) * sizeof(float));
        g_rnn_out_fifo_n -= avail;
    }
    return avail;
}

/* ============================================================
 * Top-level process.  Order: RNNoise -> AGC -> EQ.
 * ============================================================ */

int radae_micdsp_process(float* buf, int n_in)
{
    if (!g_ready || n_in <= 0) return n_in;

    int rn_on  = (int)_InterlockedAnd(&g_rnnoise_enable, 1);
    int agc_on = (int)_InterlockedAnd(&g_agc_enable, 1);
    int eq_on  = (int)_InterlockedAnd(&g_eq_enable, 1);

    int n = n_in;

    if (rn_on)
    {
        /* RNNoise drains in 480-sample (10 ms @ 48 k) frames and
         * discards the first frame after every reset / reset-equivalent
         * (g_first_rnnoise_frame priming).  During the priming gap the
         * call returns fewer samples than it consumed -- the rest are
         * buffered inside RNNoise's input FIFO and surface on later
         * calls.  Propagate the actual count downstream instead of
         * zero-padding: zero-padding would force LPCNet to encode
         * silence for the missing samples, which is the source of the
         * "swallowed first syllable" at the start of every over.
         * Matches freedv-gui's RNNoiseStep contract. */
        n = rnnoise_step_block(buf, n);
        if (n <= 0) return 0;
    }

    eq_rebuild_if_dirty();

    if (agc_on)
    {
        /* Same contract as RNNoise: AGC drains in 10 ms blocks at the
         * configured sample rate; during startup priming the output
         * count is less than the input.  Propagate the actual count. */
        n = agc_step_block(buf, n);
        if (n <= 0) return 0;
    }

    if (eq_on || g_eq_vol.enabled)
        eq_step_block(buf, n);

    return n;
}
