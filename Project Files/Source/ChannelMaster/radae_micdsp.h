/*  radae_micdsp.h
 *
 *
 *  Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */

#ifndef _radae_micdsp_h
#define _radae_micdsp_h

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Lifecycle.  Called from create_radae / destroy_radae.
 * sample_rate is the radio's audio block rate (= outrate in
 * radae.c, typically 48000).
 * ============================================================ */
void radae_micdsp_create(int sample_rate);
void radae_micdsp_destroy(void);

/* Run the chain in-place on a mono float block at sample_rate.
 * Each enabled stage runs in order: RNNoise -> AGC -> EQ.
 * RNNoise has its own internal 480-sample FIFO so callers may pass
 * any block size; the FIFO bridges RNNoise's fixed frame size to
 * the radio's audio-block size.  Output samples emerge at the same
 * rate but with up to 10 ms of additional latency (RNNoise frame).
 *
 *   buf       in/out -- mono floats, scaled in [-1, +1]
 *   n_in      number of input samples
 *   returns   number of output samples actually emitted (may be 0
 *             during RNNoise startup priming or while the FIFO is
 *             not yet full).
 */
int  radae_micdsp_process(float* buf, int n_in);

/* ============================================================
 * Setters (UI-driven).  All thread-safe via volatile reads on the
 * audio-thread side; UI thread does plain writes.  EQ filter rebuilds
 * happen lazily on the audio thread when a parameter changes.
 * ============================================================ */

/* RNNoise enable.  Forced 48 kHz; if create_sample_rate != 48000
 * the block is internally resampled (48 kHz <-> sample_rate). */
void radae_micdsp_set_rnnoise_enabled(int enable);

/* AGC enable + LUFS target (-30..-15 dB, default -23 LUFS to match
 * FreeDV-GUI). */
void radae_micdsp_set_agc_enabled(int enable);
void radae_micdsp_set_agc_target_lufs(double target_lufs);

/* EQ master enable -- bass/mid/treble bands run only when this is
 * on.  Vol stage is independent (always runs whenever its dB != 0). */
void radae_micdsp_set_eq_enabled(int enable);

/* EQ band parameters.  freq in Hz, gain in dB, Q for the mid
 * peaking band.  Vol in dB (linear unity = 0 dB). */
void radae_micdsp_set_eq_bass(double freq_hz, double gain_db);
void radae_micdsp_set_eq_mid (double freq_hz, double gain_db, double q);
void radae_micdsp_set_eq_treble(double freq_hz, double gain_db);
void radae_micdsp_set_eq_vol(double gain_db);

/* Reset all stage state.  Called from the MOX RX->TX edge in radae.c
 * so each over starts deterministic: RNNoise first-frame re-primed,
 * AGC current/target gain zeroed and ebur128 + WebRTC limiter state
 * reinitialised, biquad z-state cleared. */
void radae_micdsp_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* _radae_micdsp_h */
