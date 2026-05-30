/*  radae.h

    Wrapper module that integrates the RADE V1 (Radio AutoEncoder) digital
    voice modem (the in-repo radae_c library) into Thetis.

    Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
    RADE V1: BSD-2-Clause, David Rowe. radae_c ported with reference to
    Peter B Marks' radae_nopy (BSD-2-Clause).

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
 * by Thetis's setup UI).  Dual-RX: all RX-side getters/setters take
 * an `int rx` argument (0 = RX1, 1 = RX2).  TX-side and global PORTs
 * stay parameterless.
 * ============================================================ */

/* RX master enable, per receiver. */
PORT void  SetRadaeRxEnabled(int rx, int enable);
PORT int   GetRadaeRxEnabled(int rx);

/* TX master enable.  Single -- the TXA chain is single. */
PORT void  SetRadaeTxEnabled(int enable);
PORT int   GetRadaeTxEnabled(void);

/* Per-RX status getters for UI meters / sync indicator. */
PORT int   GetRadaeSync(int rx);          /* 0/1 */
PORT int   GetRadaeSnrDb(int rx);         /* dB SNR estimate */
PORT int   GetRadaeRxLevelDb(int rx);     /* peak |sample| as dBFS, -120 if no signal */
PORT int   GetRadaeClip(int rx);          /* 1 if any block-peak hit clip in last ~500 ms */

/* Per-RX most recently decoded remote callsign. */
PORT int   GetRadaeRemoteCallsign(int rx, char* dst, int max);
PORT int   GetRadaeRemoteCallsignSeq(int rx);
PORT int   GetRadaeEooDecodePulse(int rx);

/* TX-side mic tap meters (single). */
PORT int   GetRadaeTxMicLevelDb(void);
PORT int   GetRadaeTxMicClip(void);

/* Per-RX dial frequency offset. */
PORT float GetRadaeFreqOffset(int rx);
PORT void  SetRadaeFreqOffset(int rx, float hz);

/* TX-side MOX edge notifications (single). */
PORT void  RadaeNotifyEndOfOver(void);
PORT void  RadaeNotifyBeginOver(void);

/* EOO-safe un-key handshake (PTTRADE arbiter).  GetRadaeEooFlushed returns 1
 * once the EOO + 60ms trailing silence have left mic_io.  SetRadaeTxSilenceHold
 * keeps the TX gate writing silence while the radio is held keyed during flush. */
PORT int   GetRadaeEooFlushed(void);
PORT void  SetRadaeTxSilenceHold(int on);

/* TX-side outgoing EOO callsign (single -- one operator identity). */
PORT void  SetRadaeEooCallsign(const char* callsign);

/* Per-RX loopback toggle.  When ON, the TX-side encoder output is
 * routed into THIS RX's decoder input via a dedicated bridge ring;
 * mic_io stays silent while any RX has loopback enabled. */
PORT void  SetRadaeLoopbackEnabled(int rx, int enable);
PORT int   GetRadaeLoopbackEnabled(int rx);

/* MOX-state mirror, pushed by audio.cs on every MOX edge (global). */
PORT void  SetRadaeMoxState(int mox);

/* Sample-amplitude scaling.  Mic scale is TX-side (single).
 * RX scales are per-RX (one set per decoder). */
PORT void  SetRadaeMicScale(double scale);
PORT void  SetRadaeRxScale(int rx, double scale);
PORT void  SetRadaeRxDialScale(int rx, double scale);

/* Per-RX AF post-decode multiplier, captured at the C# side and
 * applied in pipe.c after xradae_rx returns. */
PORT void  SetRadaeRxAFGain(int rx, double gain);
PORT float GetRadaeRxAFGain(int rx);

/* Pre-encoder mic conditioning (TX-side, single). */
PORT void  SetRadaeMicRNNoiseEnabled(int enable);
PORT void  SetRadaeMicAGCEnabled(int enable);
PORT void  SetRadaeMicAGCTargetLufs(double target_lufs);
PORT void  SetRadaeMicEQEnabled(int enable);
PORT void  SetRadaeMicEQBass(double freq_hz, double gain_db);
PORT void  SetRadaeMicEQMid (double freq_hz, double gain_db, double q);
PORT void  SetRadaeMicEQTreble(double freq_hz, double gain_db);
PORT void  SetRadaeMicEQVol(double gain_db);

/* Diagnostic bypass flags (TX-side, single). */
PORT void  SetRadaeBypassMicDsp(int enable);
PORT void  SetRadaeBypassEncoderCore(int enable);
PORT void  SetRadaeBypassRmatch(int enable);
PORT void  SetRadaeBypassEncoder(int enable);
PORT void  SetRadaeBypassAll(int enable);

/* ============================================================
 * Hot-path entry points called from xpipe() in pipe.c.  xradae_rx
 * receives the rx index; xradae_tx is single.
 * ============================================================ */

void xradae_rx(int rx, double* rbuff_io);
void xradae_tx(double* mic_io);

#ifdef __cplusplus
}
#endif

#endif /* _radae_h */
