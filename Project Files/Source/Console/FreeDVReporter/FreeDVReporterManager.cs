/*  FreeDVReporterManager.cs
 *
 *
 *  Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
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

using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace Thetis.FreeDVReporter
{
    public static class FreeDVReporterManager
    {
        private static FreeDVReporterClient _client;
        private static FreeDVReporterForm   _form;
        private static Console              _console;

        private static Console.MoxChanged             _moxHandler;
        private static Console.VFOAFrequencyChanged   _vfoaHandler;
        private static Console.TuneChanged            _tuneHandler;
        private static Console.TwoToneChanged         _twoToneHandler;

        /* Last value we sent to the server.  Recomputed from
         * MOX && !TUN && !TwoTone whenever any of those flips. */
        private static bool _lastReportedTransmitting;

        /* RADAE rx-poll: fires every tick (1 Hz) while sync==1.  Polled
         * from a Timer so we don't add another hot subscription to the
         * audio thread.  The decoded RX callsign is now reliable
         * (FreeDV-GUI rade_text codec, LDPC + CRC-validated), so we
         * surface it both on the wire (rx_report.callsign) and in the
         * self-row mirror.  Cleared on sync 1->0 so a stale callsign
         * from a previous over isn't broadcast at the start of a new
         * sync session. */
        private static System.Windows.Forms.Timer _radaePollTimer;
        private static int    _lastSync          = 0;
        private static int    _lastCallsignSeq   = 0;
        private static string _lastDecodedCall   = "";

        private const string MODE_TAG = "RADEV1";

        /* When true, Thetis publishes its own data (callsign, freq, TX,
         * RX SNR) to qso.freedv.org as a "report" client.  When false,
         * the underlying socket connects with role="view" so the live
         * station list still shows but no own data is sent.  Toggled
         * live via SetReportingEnabled. */
        private static bool _reportingEnabled = true;

        public static bool IsEnabled { get { return _client != null; } }
        public static FreeDVReporterClient Client { get { return _client; } }

        /* Last VFOA frequency we pushed to the server, in Hz.  The form
         * polls this every refresh tick so "Track band" mode follows the
         * radio without us re-firing events. */
        public static ulong CurrentFrequencyHz { get; private set; }

        public static Action OnFormClosedByUser;   /* set by Setup so the X
                                                     button can untick chkRADAEReporter */

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern void OutputDebugStringW([MarshalAs(UnmanagedType.LPWStr)] string lpOutputString);

        private static void Log(string s)
        {
            try { OutputDebugStringW("[FreeDVReporter] mgr: " + s + "\n"); } catch { }
        }

        public static void Enable(Console console, string callsign, string grid, string msg, bool reportingEnabled)
        {
            Log("Enable() call='" + callsign + "' grid='" + grid + "' msg-len=" + (msg ?? "").Length + " reporting=" + reportingEnabled);
            _reportingEnabled = reportingEnabled;
            if (_client != null) { Log("Enable: client already exists, applying identity"); ApplyIdentity(callsign, grid, msg); ShowForm(console); return; }

            _console = console;
            _client = new FreeDVReporterClient
            {
                Callsign   = (callsign ?? "").Trim().ToUpperInvariant(),
                GridSquare = (grid     ?? "").Trim().ToUpperInvariant(),
                ClientName = "Thetis-rade 2.10.3.16",
                RxOnly     = false,
                Role       = _reportingEnabled ? "report" : "view",
            };

            _client.Start();

            /* Always prime the client's cached state (_lastFreqHz,
             * _lastTransmitting, _lastMessage) regardless of reporting
             * mode.  In view mode FireAndForget suppresses the wire
             * send -- but the cache is what ConnectAndPumpAsync step 4
             * re-publishes on a future role flip to "report".  Without
             * this priming, toggling chkRADAEReporting on after Enable
             * would re-connect with empty msg / freq / tx state. */
            try
            {
                CurrentFrequencyHz = (ulong)Math.Round(console.VFOAFreq * 1e6);
                _client.EmitFreqChange(CurrentFrequencyHz);
                _lastReportedTransmitting = ComputeRealTx(console);
                _client.EmitTxReport(MODE_TAG, _lastReportedTransmitting);
                if (!string.IsNullOrEmpty(msg)) _client.EmitMessageUpdate(msg);
            }
            catch { }

            /* Real TX = MOX is on AND user is not tuning AND not running
             * the 2-tone IMD test.  Any of MOX / TUN / 2TON flipping
             * recomputes the state; we only emit when it actually
             * changes so the server isn't spammed. */
            _moxHandler     = (rx, oldMox, newMox)        => RecomputeTx(console);
            _tuneHandler    = (rx, oldTune, newTune)      => RecomputeTx(console);
            _twoToneHandler = (rx, oldState, newState)    => RecomputeTx(console);
            _vfoaHandler = (oldBand, newBand, oldMode, newMode, oldFilter, newFilter,
                            oldFreq, newFreq, oldCentreF, newCentreF,
                            oldCTUN, newCTUN, oldZoom, newZoom, offset, rx) =>
            {
                try
                {
                    /* Always update CurrentFrequencyHz (drives Track-band)
                     * and always call EmitFreqChange so the client's
                     * cached _lastFreqHz is current; client suppresses
                     * the wire send when role=="view". */
                    CurrentFrequencyHz = (ulong)Math.Round(newFreq * 1e6);
                    _client?.EmitFreqChange(CurrentFrequencyHz);
                }
                catch { }
            };
            console.MoxChangeHandlers           += _moxHandler;
            console.TuneChangedHandlers         += _tuneHandler;
            console.TwoToneChangedHandlers      += _twoToneHandler;
            console.VFOAFrequencyChangeHandlers += _vfoaHandler;

            _radaePollTimer = new System.Windows.Forms.Timer { Interval = 1000 };
            _radaePollTimer.Tick += OnRadaePoll;
            _radaePollTimer.Start();

            ShowForm(console);
        }

        private static bool ComputeRealTx(Console console)
        {
            try { return console.MOX && !console.TUN && !console.TwoTone; }
            catch { return false; }
        }

        /* Live toggle of the reporting flag.  Forces the underlying
         * Socket.IO client to drop and reconnect with the new role
         * (server treats role as immutable for the life of a session,
         * so a reconnect is the only way to switch). */
        public static void SetReportingEnabled(bool enabled)
        {
            if (_reportingEnabled == enabled) return;
            _reportingEnabled = enabled;
            Log("SetReportingEnabled(" + enabled + ")");
            if (_client == null) return;
            try
            {
                _client.Role = _reportingEnabled ? "report" : "view";
                /* Stop + Start to force a clean reconnect with the new
                 * auth payload.  RunLoop's reconnect-in-5s machinery is
                 * already what we want for the gap. */
                _client.Stop();
                _client.Start();
                /* Re-publish initial state on the new session if we now
                 * report. */
                if (_reportingEnabled && _console != null)
                {
                    try
                    {
                        _client.EmitFreqChange(CurrentFrequencyHz);
                        _lastReportedTransmitting = ComputeRealTx(_console);
                        _client.EmitTxReport(MODE_TAG, _lastReportedTransmitting);
                    }
                    catch { }
                }
            }
            catch { }
        }

        /* Double-click a station row in the reporter dialog -> tune
         * VFOA there and snap the operating mode to DIGL (<12 MHz) or
         * DIGU (>=12 MHz), matching the chkRADAE-enable mode rule. */
        public static void TuneToFrequency(ulong hz)
        {
            if (_console == null || hz == 0) return;
            try
            {
                double mhz = hz / 1e6;
                _console.VFOAFreq = mhz;
                DSPMode want = mhz < 12.0 ? DSPMode.DIGL : DSPMode.DIGU;
                if (_console.RX1DSPMode != want) _console.RX1DSPMode = want;
                Log("TuneToFrequency " + mhz.ToString("0.000000") + " MHz mode=" + want);
            }
            catch (Exception ex) { Log("TuneToFrequency failed: " + ex.Message); }
        }

        private static void RecomputeTx(Console console)
        {
            try
            {
                bool now = ComputeRealTx(console);
                if (now == _lastReportedTransmitting) return;   /* debounce */
                _lastReportedTransmitting = now;
                /* Always call EmitTxReport so the client's cached
                 * _lastTransmitting is current; client suppresses the
                 * wire send when role=="view". */
                _client?.EmitTxReport(MODE_TAG, now);
            }
            catch { }
        }

        public static void Disable()
        {
            Log("Disable()");
            try
            {
                if (_console != null)
                {
                    if (_moxHandler     != null) _console.MoxChangeHandlers           -= _moxHandler;
                    if (_tuneHandler    != null) _console.TuneChangedHandlers         -= _tuneHandler;
                    if (_twoToneHandler != null) _console.TwoToneChangedHandlers      -= _twoToneHandler;
                    if (_vfoaHandler    != null) _console.VFOAFrequencyChangeHandlers -= _vfoaHandler;
                }
            }
            catch { }
            _moxHandler = null; _tuneHandler = null; _twoToneHandler = null; _vfoaHandler = null;

            try { _radaePollTimer?.Stop(); _radaePollTimer?.Dispose(); } catch { }
            _radaePollTimer = null;

            try { _client?.Stop(); _client?.Dispose(); } catch { }
            _client = null;

            try
            {
                if (_form != null && !_form.IsDisposed)
                {
                    _form.OnUserClose = null;
                    _form.Close();
                    _form.Dispose();
                }
            }
            catch { }
            _form = null;
            _console = null;
        }

        public static void ApplyIdentity(string callsign, string grid, string msg)
        {
            if (_client == null) return;
            string newCall = (callsign ?? "").Trim().ToUpperInvariant();
            string newGrid = (grid     ?? "").Trim().ToUpperInvariant();
            bool identityChanged = newCall != _client.Callsign || newGrid != _client.GridSquare;
            _client.Callsign   = newCall;
            _client.GridSquare = newGrid;
            if (identityChanged)
            {
                /* Re-handshake so the server sees the new callsign / grid. */
                try { _client.Stop(); } catch { }
                _client.Start();
            }
            if (msg != null) _client.EmitMessageUpdate(msg);
        }

        public static void ShowForm(Console console)
        {
            if (_client == null) return;
            if (_form == null || _form.IsDisposed)
            {
                _form = new FreeDVReporterForm(_client);
                _form.OnUserClose = () => { OnFormClosedByUser?.Invoke(); };
            }
            /* Show without an owner so the form is a top-level window with
             * its own taskbar entry (matches FreeDV-GUI's wxFrame).  An
             * owned form would inherit the parent's taskbar slot and
             * disappear with it. */
            if (!_form.Visible) _form.Show();
            _form.BringToFront();
        }

        private static void OnRadaePoll(object sender, EventArgs e)
        {
            if (_client == null) return;
            try
            {
                int sync = cmaster.GetRadaeSync();
                int snr  = cmaster.GetRadaeSnrDb();

                /* Track the decoded callsign across the sync session.
                 * The rade_text codec only fires on validated decodes
                 * (LDPC parity + CRC8), so the seq counter ticks once
                 * per successful EOO frame.  Refresh our cache on each
                 * tick so future periodic emits carry the latest call. */
                int seq = cmaster.GetRadaeRemoteCallsignSeq();
                if (seq != _lastCallsignSeq)
                {
                    var sb = new System.Text.StringBuilder(16);
                    cmaster.GetRadaeRemoteCallsign(sb, sb.Capacity);
                    _lastDecodedCall = sb.ToString().Trim().ToUpperInvariant();
                    _lastCallsignSeq = seq;
                }

                /* On sync 1->0 transition, drop the cached call so a
                 * stale value from a previous over does not leak into
                 * the next sync session's pre-EOO rx_reports. */
                if (sync == 0 && _lastSync != 0)
                    _lastDecodedCall = "";
                _lastSync = sync;

                /* Periodic rx_report while in sync.  Fires every poll
                 * tick (1 Hz) so qso.freedv.org sees us as actively
                 * receiving, and so the local mirror's LastRxUtc keeps
                 * refreshing -- self-row stays continuously green
                 * rather than only flashing on each EOO frame.
                 *
                 * Callsign field is the most-recent EOO-decoded value
                 * (or empty before the first EOO of the current sync
                 * session).  Now reliable -- backed by the FreeDV-GUI
                 * rade_text LDPC + CRC8 codec, so we report it both on
                 * the wire and in the self-row mirror.
                 *
                 * Client.FireAndForget suppresses the wire send when
                 * role=="view", so we can call unconditionally. */
                if (sync != 0)
                {
                    _client.EmitRxReport(_lastDecodedCall, MODE_TAG, snr);
                    if (_reportingEnabled)
                        _client.LocalMirrorRxReport(_lastDecodedCall, MODE_TAG, snr);
                }
            }
            catch { }
        }
    }
}
