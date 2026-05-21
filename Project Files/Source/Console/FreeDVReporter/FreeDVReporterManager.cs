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
        private static FreeDVReporterClient _clientRx2;   // Dual-RX: second connection for RX2 reports.
        private static FreeDVReporterForm   _form;
        private static Console              _console;

        private static Console.MoxChanged             _moxHandler;
        private static Console.VFOAFrequencyChanged   _vfoaHandler;
        private static Console.VFOBFrequencyChanged   _vfobHandler;
        private static Console.TuneChanged            _tuneHandler;
        private static Console.TwoToneChanged         _twoToneHandler;

        /* Last value we sent to the server.  Recomputed from
         * MOX && !TUN && !TwoTone whenever any of those flips. */
        private static bool _lastReportedTransmitting;
        private static bool _lastReportedTransmittingRx2;
        private static ulong _currentFrequencyRx2Hz;
        // RX2 reporting (VIS) -- independent of RX1.
        private static bool _rx2ReportingEnabled = false;
        // RX2 poll-cache (parallel to the RX1 fields below).
        private static int    _lastSyncRx2        = 0;
        private static int    _lastCallsignSeqRx2 = 0;
        private static string _lastDecodedCallRx2 = "";
        private static string _rx2Message         = "";

        /* RADAE rx-poll: fires every tick (1 Hz).  Polled from a Timer
         * so we don't add another hot subscription to the audio thread.
         * The decoded RX callsign is reliable (FreeDV-GUI rade_text
         * codec, LDPC + CRC-validated), so we surface it both on the
         * wire (rx_report.callsign) and in the self-row mirror.
         * Two emit triggers:
         *   - sync==1 ticks  -> periodic refresh while receiving;
         *   - seq counter rising edge -> one shot per validated EOO
         *     decode regardless of sync (the EOO frame arrives just
         *     before sync drops to 0 at end-of-over, so the periodic
         *     emit alone misses it).
         * Cached callsign is cleared at the START of a new sync
         * session (sync 0->1 edge) rather than at the end of the
         * previous one, so the just-decoded call survives across the
         * 1 Hz poll cadence and the reporter form's RX Call column
         * keeps showing the last call between overs. */
        private static System.Windows.Forms.Timer _radaePollTimer;
        private static int    _lastSync          = 0;
        private static int    _lastCallsignSeq   = 0;
        private static string _lastDecodedCall   = "";

        private const string MODE_TAG = "RADEV1";

        /* Reported "version" / client-name string, e.g. "Thetis-RADE
         * 2.10.3.16".  Pulled once from the entry assembly so an
         * AssemblyVersion bump is automatically reflected without a
         * manual edit here.  Falls back to the executing assembly and
         * finally a literal "Thetis-RADE" if both are unavailable
         * (shouldn't happen in a normal launch). */
        private static readonly string CLIENT_NAME = BuildClientName();

        private static string BuildClientName()
        {
            try
            {
                var asm = System.Reflection.Assembly.GetEntryAssembly()
                          ?? System.Reflection.Assembly.GetExecutingAssembly();
                var v = asm?.GetName().Version;
                if (v != null)
                    return "Thetis-RADE " + v.Major + "." + v.Minor + "." + v.Build + "." + v.Revision;
            }
            catch { }
            return "Thetis-RADE";
        }

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

        /* Dual-RX: VFOB frequency mirror, kept current by the _vfobHandler
         * subscription installed in Enable().  Exposed read-only so the
         * reporter form's "Track RX2" toggle can drive its band/frequency
         * filter from VFOB the same way "Track RX1" uses CurrentFrequencyHz
         * (VFOA). */
        public static ulong CurrentFrequencyRx2Hz { get { return _currentFrequencyRx2Hz; } }

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
                ClientName = CLIENT_NAME,
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
                /* Seed the VFOB cache too so "Track RX2" has a starting
                 * frequency before the user touches VFOB.  The wire-side
                 * EmitFreqChange is harmless if _clientRx2 is still null
                 * here (the per-RX VIS opens that client later). */
                _currentFrequencyRx2Hz = (ulong)Math.Round(console.VFOBFreq * 1e6);
                _clientRx2?.EmitFreqChange(_currentFrequencyRx2Hz);
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
            // VFOB freq -> RX2 client (when up).
            _vfobHandler = (oldBand, newBand, oldMode, newMode, oldFilter, newFilter,
                            oldFreq, newFreq, oldCentreF, newCentreF,
                            oldCTUN, newCTUN, oldZoom, newZoom, offset, rx) =>
            {
                try
                {
                    _currentFrequencyRx2Hz = (ulong)Math.Round(newFreq * 1e6);
                    _clientRx2?.EmitFreqChange(_currentFrequencyRx2Hz);
                }
                catch { }
            };
            console.MoxChangeHandlers           += _moxHandler;
            console.TuneChangedHandlers         += _tuneHandler;
            console.TwoToneChangedHandlers      += _twoToneHandler;
            console.VFOAFrequencyChangeHandlers += _vfoaHandler;
            console.VFOBFrequencyChangeHandlers += _vfobHandler;

            _radaePollTimer = new System.Windows.Forms.Timer { Interval = 1000 };
            _radaePollTimer.Tick += OnRadaePoll;
            _radaePollTimer.Start();

            ShowForm(console);
        }

        private static bool ComputeRealTx(Console console)
        {
            try
            {
                if (!console.MOX || console.TUN || console.TwoTone) return false;
                // RADE-bypass over: when transmitting on VFO B with RX2
                // enabled while chkRADAE is on, audio.cs bypasses the
                // encoder and this over goes out as plain SSB voice on
                // RX2's frequency -- not a RADE transmission, so it
                // should not be reported to qso.freedv.org.  Matches
                // the per-over predicate in audio.cs::MOX setter.
                if (console.RadaeEnabled && console.RX2Enabled && console.VFOBTX)
                    return false;
                return true;
            }
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
         * VFOA there and snap the operating mode, matching the
         * chkRADAE-enable rule:
         *   5.0 - 5.5 MHz (60 m) -> DIGU (regulatory USB convention)
         *   <12 MHz              -> DIGL
         *   >=12 MHz             -> DIGU
         */
        public static void TuneToFrequency(ulong hz)
        {
            if (_console == null || hz == 0) return;
            try
            {
                double mhz = hz / 1e6;
                _console.VFOAFreq = mhz;
                DSPMode want;
                if (mhz >= 5.0 && mhz < 5.5)   want = DSPMode.DIGU;
                else if (mhz < 12.0)           want = DSPMode.DIGL;
                else                           want = DSPMode.DIGU;
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

                /* RX2 TX-report: the symmetric predicate -- RX2 reports
                 * "transmitting" only when transmitting via VFO B on RX2
                 * with chkRADAERX2 on. */
                if (_clientRx2 != null)
                {
                    bool rx2Now = ComputeRealTxRx2(console);
                    if (rx2Now != _lastReportedTransmittingRx2)
                    {
                        _lastReportedTransmittingRx2 = rx2Now;
                        _clientRx2.EmitTxReport(MODE_TAG, rx2Now);
                    }
                }
            }
            catch { }
        }

        private static bool ComputeRealTxRx2(Console c)
        {
            try
            {
                if (!c.MOX || c.TUN || c.TwoTone) return false;
                // RX2 reports TX only when VFO B is selected, RX2 is enabled,
                // and RX2-side RADE is active.  All other overs (VFO A,
                // VFO B with RX2 disabled, etc.) are RX1's concern.
                if (!c.RadaeRx2Enabled) return false;
                if (!c.RX2Enabled || !c.VFOBTX) return false;
                return true;
            }
            catch { return false; }
        }

        /* ===== Dual-RX additions ===== */

        /* Set RX2's reporter message.  When the RX2 client is up, the
         * message is published immediately; otherwise it is cached for
         * the next Enable cycle. */
        public static void SetRx2Message(string msg)
        {
            _rx2Message = msg ?? "";
            try { _clientRx2?.EmitMessageUpdate(_rx2Message); } catch { }
        }

        /* RX2 reporting (VIS) enable.  When true, opens (or maintains) a
         * second connection to qso.freedv.org dedicated to RX2 frequency
         * + SNR.  When false, closes that connection.  The primary
         * RX1 connection is unaffected and uses chkRADAEReporting (RX1
         * VIS) -- the two are independent. */
        public static void SetRx2ReportingEnabled(bool enabled)
        {
            if (_rx2ReportingEnabled == enabled) return;
            _rx2ReportingEnabled = enabled;
            Log("SetRx2ReportingEnabled(" + enabled + ")");
            if (_console == null) return;

            if (enabled)
            {
                if (_clientRx2 == null)
                {
                    _clientRx2 = new FreeDVReporterClient
                    {
                        Callsign   = _client != null ? _client.Callsign   : "",
                        GridSquare = _client != null ? _client.GridSquare : "",
                        ClientName = CLIENT_NAME,
                        RxOnly     = false,
                        Role       = "report",
                        // Discard inbound rx_report on the RX2 connection --
                        // the primary RX1 connection already builds the
                        // station list; we don't need it duplicated.
                        IgnoreInboundStations = true,
                    };
                    _clientRx2.Start();
                    try
                    {
                        _currentFrequencyRx2Hz = (ulong)Math.Round(_console.VFOBFreq * 1e6);
                        _clientRx2.EmitFreqChange(_currentFrequencyRx2Hz);
                        _lastReportedTransmittingRx2 = ComputeRealTxRx2(_console);
                        _clientRx2.EmitTxReport(MODE_TAG, _lastReportedTransmittingRx2);
                        if (!string.IsNullOrEmpty(_rx2Message))
                            _clientRx2.EmitMessageUpdate(_rx2Message);
                    }
                    catch { }
                }
            }
            else
            {
                try { _clientRx2?.Stop(); _clientRx2?.Dispose(); } catch { }
                _clientRx2 = null;
            }
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
                    if (_vfobHandler    != null) _console.VFOBFrequencyChangeHandlers -= _vfobHandler;
                }
            }
            catch { }
            _moxHandler = null; _tuneHandler = null; _twoToneHandler = null; _vfoaHandler = null; _vfobHandler = null;

            try { _radaePollTimer?.Stop(); _radaePollTimer?.Dispose(); } catch { }
            _radaePollTimer = null;

            try { _client?.Stop(); _client?.Dispose(); } catch { }
            _client = null;
            try { _clientRx2?.Stop(); _clientRx2?.Dispose(); } catch { }
            _clientRx2 = null;

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
                /* Pass the console explicitly -- Console.getConsole() is
                 * not safe here at the very first startup because the
                 * Console ctor is still on the stack (its _theConsole
                 * static assignment happens after the ctor returns), and
                 * this form is constructed transitively via Setup's
                 * post-init rehydrate of chkRADAEReporter inside that
                 * Console ctor. */
                _form = new FreeDVReporterForm(_client, _console);
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
                int sync = cmaster.GetRadaeSync(0);
                int snr  = cmaster.GetRadaeSnrDb(0);

                /* Track the decoded callsign across the sync session.
                 * The rade_text codec only fires on validated decodes
                 * (LDPC parity + CRC8), so the seq counter ticks once
                 * per successful EOO frame.  A change between polls
                 * means a fresh EOO has just been decoded -- we use
                 * that edge below to drive the canonical "I received
                 * <callsign>" rx_report independent of the sync gate. */
                int seq = cmaster.GetRadaeRemoteCallsignSeq(0);
                bool freshDecode = seq != _lastCallsignSeq;
                if (freshDecode)
                {
                    var sb = new System.Text.StringBuilder(16);
                    cmaster.GetRadaeRemoteCallsign(0, sb, sb.Capacity);
                    _lastDecodedCall = sb.ToString().Trim().ToUpperInvariant();
                    _lastCallsignSeq = seq;
                }

                /* Clear the cached callsign at the START of a new sync
                 * session (0->1 edge), not at the end of the previous
                 * one (1->0).  Two reasons:
                 *  - the EOO frame arrives just before sync drops to
                 *    0, so a 1->0 clear would erase the decoded call
                 *    before the next 1 Hz poll can ship it on the
                 *    wire / mirror it into the self-row;
                 *  - keeps the reporter form's RX Call column
                 *    populated with the last decoded call between
                 *    overs, which is the user-visible behaviour we
                 *    want.  A new session overwrites it as soon as
                 *    that session's first EOO decodes. */
                if (sync != 0 && _lastSync == 0)
                    _lastDecodedCall = "";
                _lastSync = sync;

                /* Emit rx_report whenever either:
                 *   - we are currently in sync (periodic refresh so
                 *     qso.freedv.org sees us actively receiving and
                 *     the self-row LimeGreen tail keeps re-arming), or
                 *   - we just decoded a fresh EOO callsign (the
                 *     canonical "I received <call>" event -- must
                 *     reach the server even when sync has already
                 *     dropped at the EOO boundary, which is the
                 *     normal case at end-of-over).
                 *
                 * Callsign field is the most-recent EOO-decoded value
                 * (or empty before the first EOO of the current sync
                 * session).  Backed by the FreeDV-GUI rade_text LDPC
                 * + CRC8 codec, so we report it both on the wire and
                 * in the self-row mirror.
                 *
                 * Client.FireAndForget suppresses the wire send when
                 * role=="view"; we still locally mirror only when
                 * reporting is enabled. */
                if (sync != 0 || freshDecode)
                {
                    _client.EmitRxReport(_lastDecodedCall, MODE_TAG, snr);
                    if (_reportingEnabled)
                        _client.LocalMirrorRxReport(_lastDecodedCall, MODE_TAG, snr);
                }

                /* Dual-RX: when the RX2 client is up, poll RX2's sync/SNR/
                 * callsign symmetrically and emit through the second
                 * connection. */
                if (_clientRx2 != null)
                {
                    int sync2 = cmaster.GetRadaeSync(1);
                    int snr2  = cmaster.GetRadaeSnrDb(1);
                    int seq2  = cmaster.GetRadaeRemoteCallsignSeq(1);
                    bool fresh2 = seq2 != _lastCallsignSeqRx2;
                    if (fresh2)
                    {
                        var sb2 = new System.Text.StringBuilder(16);
                        cmaster.GetRadaeRemoteCallsign(1, sb2, sb2.Capacity);
                        _lastDecodedCallRx2 = sb2.ToString().Trim().ToUpperInvariant();
                        _lastCallsignSeqRx2 = seq2;
                    }
                    if (sync2 != 0 && _lastSyncRx2 == 0)
                        _lastDecodedCallRx2 = "";
                    _lastSyncRx2 = sync2;
                    if (sync2 != 0 || fresh2)
                    {
                        _clientRx2.EmitRxReport(_lastDecodedCallRx2, MODE_TAG, snr2);
                    }
                }
            }
            catch { }
        }
    }
}
