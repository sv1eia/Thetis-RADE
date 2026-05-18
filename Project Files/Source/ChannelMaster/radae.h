/*  radae.h

    Wrapper module that integrates the RADE V1 (Radio AutoEncoder) digital
    voice modem (https://github.com/peterbmarks/radae_nopy) into Thetis.

    Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
    radae_nopy upstream: BSD-2-Clause, David Rowe / Peter B Marks.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA  02110-1301  USA
*/

#ifndef _radae_h
#define _radae_h

/* PORT export macro from cmcomm.h's neighbours; fall back if standalone */
#ifndef PORT
#define PORT __declspec(dllexport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Lifecycle
 *   Called once during ChannelMaster bring-up / tear-down,
 *   from create_pipe() / destroy_pipe().
 * ============================================================ */

void  create_radae(void);
void  destroy_radae(void);

/* ============================================================
 * Public C# API (PORT-exported from ChannelMaster.dll, P/Invoked
 * by Thetis's chkFreeDV checkbox handler).
 * ============================================================ */

/* Master enable. When enable == 0, both xradae_rx and xradae_tx are
 * pure pass-throughs (zero copy of the input buffer back into itself).
 * Toggling the flag is interlocked-atomic; safe to call from the GUI
 * thread while the audio threads are running. */
PORT void  SetRadaeRxEnabled(int enable);
PORT void  SetRadaeTxEnabled(int enable);

/* Status getters for UI meters / sync indicator. Returned values are
 * stale-tolerant — they may lag the audio thread by one block. */
PORT int   GetRadaeRxEnabled(void);
PORT int   GetRadaeTxEnabled(void);
PORT int   GetRadaeSync(void);          /* 0/1 */
PORT int   GetRadaeSnrDb(void);         /* dB SNR estimate */
PORT int   GetRadaeRxLevelDb(void);     /* peak |sample| of decoder input as dBFS (e.g. -60..0); -120 if no signal */
PORT int   GetRadaeClip(void);          /* 1 if any block-peak hit clip (>=0.8 fullscale) within the last ~500 ms, else 0 */

/* Most recently decoded callsign from a received EOO frame.  Copies
 * up to (max-1) chars + null terminator into dst.  Returns the number
 * of chars written (not counting null), or 0 if no callsign has been
 * decoded yet since the master enable was toggled. */
PORT int   GetRadaeRemoteCallsign(char* dst, int max);

/* Monotonic counter incremented every time a remote callsign is
 * successfully decoded from an EOO frame.  Caller observes a change
 * to know "fetch the callsign now and act on it" -- avoids re-firing
 * on every poll for the same already-handled decode event. */
PORT int   GetRadaeRemoteCallsignSeq(void);

/* 1 for ~500 ms after each successful EOO callsign decode, else 0.
 * Polled by the RADE EOO Decodes pulse meter. */
PORT int   GetRadaeEooDecodePulse(void);

/* TX-side mic level after the g_radae_mic_scale multiplier (driven
 * by the "RADE Mic level" spinner in Setup -> Audio -> Options).
 * Tap is at the encoder input -- equivalent to FreeDV-GUI's "Frm Mic"
 * scope tap.  Published per audio block during MOX;
 * -120 dBFS = silent. */
PORT int   GetRadaeTxMicLevelDb(void);

/* 1 for ~500 ms after the TX mic peak crossed 0.8 of fullscale (the
 * same FROM_MIC_MAX threshold FreeDV-GUI uses).  Else 0. */
PORT int   GetRadaeTxMicClip(void);
PORT float GetRadaeFreqOffset(void);    /* Hz; non-zero only when sync */

/* Optional: user dial-correction in Hz, applied to RX before rade_rx. */
PORT void  SetRadaeFreqOffset(float hz);

/* Called by Thetis on MOX -> RX transition. The next xradae_tx() call
 * after this will flush a single rade_tx_eoo() frame plus a short
 * silence trailer and then idle. */
PORT void  RadaeNotifyEndOfOver(void);

/* Called by Thetis on RX -> MOX transition. The next xradae_tx() call
 * after this fires the audit logs for items #4, #5, #6 and #9 so the
 * MOX-on edge is visible in DebugView. (Without this hook the audit
 * heuristic of "long gap between xradae_tx calls" never fires because
 * xradae_tx is invoked continuously regardless of MOX state.) */
PORT void  RadaeNotifyBeginOver(void);

/* Optional: set 8-character callsign embedded in the EOO frame so the
 * receiving station's UI can display "from CALLSIGN". */
PORT void  SetRadaeEooCallsign(const char* callsign);

/* RADE Loopback Test.  When BOTH the master chkRADAE flag (set via
 * SetRadaeRxEnabled / SetRadaeTxEnabled) AND this flag are 1, the
 * encoder's modem audio is taken from the rmatchV output (the post-
 * encoder, post-r8brain, post-rmatchV stream that would normally fill
 * mic_io) and routed into the decoder's xresampleFV input on its
 * 48 kHz side.  mic_io stays silent so the radio does not transmit.
 * The receiving station hears the encoded-then-decoded version of its
 * own mic, and the SNR estimate is visible in DebugView through the
 * existing per-second rx_diag line. */
PORT void  SetRadaeLoopbackEnabled(int enable);
PORT int   GetRadaeLoopbackEnabled(void);

/* Sample-amplitude scaling.  The C# Setup tab pushes the linear scale
 * factor (= 10^(dB/20)) of the VAC1 TXGain / RXGain spinners here so
 * the user can drive the radae encoder mic input and the radae decoder
 * input level the same way FreeDV-GUI's Mic level slider drives its
 * mic stream.  Default 1.0 (0 dB) -- no change. */
PORT void  SetRadaeMicScale(double scale);
PORT void  SetRadaeRxScale(double scale);
PORT void  SetRadaeRxDialScale(double scale);

/* RX1 AF post-decode multiplier.  Captured at the C# side and applied
 * in pipe.c after xradae_rx returns. */
PORT void  SetRadaeRx1AFGain(double gain);
PORT float GetRadaeRx1AFGain(void);

/* Pre-encoder mic conditioning (FreeDV-GUI parity).  Each stage has an
 * enable + parameters.  All three off by default.  Chain runs in
 * radae_micdsp.c as RNNoise -> AGC -> 3-band biquad EQ + Vol, in
 * series, in xradae_tx step 1 (after deswizzle, before r8brain
 * 48k -> 16k).  Effect only when chkRADAE is on (the surrounding
 * xradae_tx returns early when RADE TX is disabled). */
PORT void  SetRadaeMicRNNoiseEnabled(int enable);
PORT void  SetRadaeMicAGCEnabled(int enable);
PORT void  SetRadaeMicAGCTargetLufs(double target_lufs);
PORT void  SetRadaeMicEQEnabled(int enable);
PORT void  SetRadaeMicEQBass(double freq_hz, double gain_db);
PORT void  SetRadaeMicEQMid (double freq_hz, double gain_db, double q);
PORT void  SetRadaeMicEQTreble(double freq_hz, double gain_db);
PORT void  SetRadaeMicEQVol(double gain_db);

/* Diagnostic bypass flags (each 0 = normal, 1 = bypass).  Used by the
 * four checkboxes under Setup -> DSP -> RADE -> Diagnostics to isolate
 * which stage of the encoder pipeline is producing the residual skirt
 * splatter / receiver-decoder clicking.  Boot-OFF is enforced on the
 * C# side (non-persistent checkboxes); each flag takes effect on the
 * next xradae_tx() call. */
PORT void  SetRadaeBypassMicDsp(int enable);       /* skip Stage 1b */
PORT void  SetRadaeBypassEncoderCore(int enable);  /* skip Stage 4 -- push silence to modem FIFO */
PORT void  SetRadaeBypassRmatch(int enable);       /* skip Stage 7 -- direct copy outrate FIFO -> mic_io */
PORT void  SetRadaeBypassEncoder(int enable);      /* skip Stages 2..7 -- mic -> mic_io as SSB voice */
PORT void  SetRadaeBypassAll(int enable);          /* Test G -- xradae_tx returns at top; no read or write of mic_io */

/* ============================================================
 * Hot-path entry points called from xpipe() in pipe.c.
 *
 * RX splice point: same place the audio reaches the speaker chain.
 *   xradae_rx: takes the post-WDSP-demod audio (filter, AGC, NR, NB,
 *              SAM/AM/SSB demod -- all the user's RX enhancements
 *              already applied), feeds it to the RADE decoder, and
 *              writes the FARGAN-synthesised speech back in place.
 *
 * TX splice point: same place the mic audio leaves user-controlled
 *                  enhancements. The mic still passes through TXEQ /
 *                  Compander / CFC / Phase Rotator / Leveler before
 *                  reaching xradae_tx, so the user's mic processing
 *                  is preserved exactly. xradae_tx then replaces the
 *                  mic samples with rade_tx-encoded modem audio.
 *                  The downstream WDSP fexchange0 turns that audio
 *                  into the SSB IQ that the rig transmits.
 *
 * Mode and filter selection are NOT changed by this layer; the user
 * picks RX/TX mode and bandwidth as usual.
 *
 * In this stub revision the bodies pass through unchanged.
 * ============================================================ */

void xradae_rx(int rx, double* rbuff_io);
void xradae_tx(double* mic_io);

#ifdef __cplusplus
}
#endif

#endif /* _radae_h */
