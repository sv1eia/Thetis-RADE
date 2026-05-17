/*  FreeDVReporterClient.cs
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
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace Thetis.FreeDVReporter
{
    public class StationInfo
    {
        public string Sid;
        public string Callsign = "";
        public string GridSquare = "";
        public string Version = "";
        public bool   RxOnly;
        public ulong  FrequencyHz;
        public string Mode = "";
        public bool   Transmitting;
        public string Message = "";
        public DateTime LastUpdateUtc = DateTime.MinValue;
        public DateTime ConnectTimeUtc = DateTime.MinValue;
        public DateTime? LastTxUtc;

        /* Most-recent rx_report from this station.  The protocol carries
         * this as an event with the *received* callsign, mode and SNR --
         * we surface it on the row of the *reporting* station so the UI
         * shows "<station> heard <last-rx-callsign> @ <snr> dB". */
        public string LastRxCallsign = "";
        public string LastRxMode = "";
        public double LastRxSnr = double.NaN;
        public DateTime? LastRxUtc;

        /* Time we most recently observed transmitting==true.  Used by
         * the form to keep the red shade for 5 s after TX stops. */
        public DateTime? LastTxActiveUtc;

        /* Time the Message string most recently changed value.  Used by
         * the form for a 5 s purple "msg-update" flash, mirroring the
         * FreeDV-GUI msgRowBackgroundColor highlight. */
        public DateTime? LastMessageChangeUtc;
    }

    public class FreeDVReporterClient : IDisposable
    {
        public const int PROTOCOL_VERSION = 2;

        public string Hostname { get; set; } = "qso.freedv.org";
        public bool   UseTls   { get; set; } = false;

        public string Role       { get; set; } = "report";   /* "report" | "report_wo" | "view" */
        public string Callsign   { get; set; } = "";
        public string GridSquare { get; set; } = "";
        public string ClientName { get; set; } = "Thetis";   /* sent as "version" field */
        public bool   RxOnly     { get; set; } = false;
        public string Os         { get; set; } = "Windows";

        public ConcurrentDictionary<string, StationInfo> Stations { get; } =
            new ConcurrentDictionary<string, StationInfo>(StringComparer.Ordinal);

        public event EventHandler StationsChanged;
        public event EventHandler<string> ConnectionStateChanged;
        public event EventHandler<string> LogMessage;

        public string ConnectionState { get; private set; } = "Disconnected";

        /* Our own session id, parsed from the Socket.IO CONNECT-ack
         * payload "{\"sid\":\"...\"}".  Used by LocalMirrorRxReport to
         * route self-emitted events into our own row in the Stations
         * dictionary -- the qso.freedv.org server does not echo our
         * own rx_reports back to us, so we synthesize them locally. */
        public string MySid { get; private set; }

        private CancellationTokenSource _cts;
        private Task _runTask;
        private ClientWebSocket _ws;
        private readonly SemaphoreSlim _sendLock = new SemaphoreSlim(1, 1);

        /* Last-known outbound state.  Re-emitted after every reconnect so
         * the server's view of us is consistent. */
        private ulong  _lastFreqHz;
        private string _lastMode = "";
        private bool   _lastTransmitting;
        private string _lastMessage = "";
        private bool   _haveFreq, _haveTxState, _haveMessage;

        private int _pingIntervalMs = 25000;
        private int _pingTimeoutMs  = 20000;

        public void Start()
        {
            if (_runTask != null && !_runTask.IsCompleted) { Log("Start() ignored -- already running"); return; }
            Log("Start() host=" + Hostname + " role=" + Role + " call='" + Callsign + "' grid='" + GridSquare + "'");
            _cts = new CancellationTokenSource();
            _runTask = Task.Run(() => RunLoop(_cts.Token));
        }

        public void Stop()
        {
            try { _cts?.Cancel(); } catch { }
            try
            {
                if (_ws != null && _ws.State == WebSocketState.Open)
                {
                    _ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye",
                                   CancellationToken.None).Wait(1000);
                }
            }
            catch { }
            try { _runTask?.Wait(2000); } catch { }
            _runTask = null;
        }

        public void Dispose() { Stop(); _cts?.Dispose(); _ws?.Dispose(); _sendLock?.Dispose(); }

        public void EmitFreqChange(ulong hz)
        {
            _lastFreqHz = hz; _haveFreq = true;
            var p = new JObject { ["freq"] = hz };
            FireAndForget("freq_change", p);
        }

        public void EmitTxReport(string mode, bool transmitting)
        {
            _lastMode = mode ?? ""; _lastTransmitting = transmitting; _haveTxState = true;
            var p = new JObject { ["mode"] = _lastMode, ["transmitting"] = transmitting };
            FireAndForget("tx_report", p);
        }

        public void EmitRxReport(string callsign, string mode, int snr)
        {
            var p = new JObject
            {
                ["callsign"] = callsign ?? "",
                ["mode"]     = mode ?? "",
                ["snr"]      = snr,
            };
            FireAndForget("rx_report", p);
        }

        /* Local-only mirror of an rx_report.  Updates our own row in
         * the Stations dictionary as if the server had echoed an
         * rx_report for us back to us.  qso.freedv.org does NOT echo
         * our rx_reports back -- so without this our self-row never
         * shows the green flash on RX activity, and the locally
         * decoded callsign never appears anywhere on screen. */
        public void LocalMirrorRxReport(string heardCallsign, string mode, int snr)
        {
            if (string.IsNullOrEmpty(MySid)) return;
            var p = new JObject
            {
                ["sid"]         = MySid,
                ["callsign"]    = heardCallsign ?? "",
                ["mode"]        = mode ?? "",
                ["snr"]         = snr,
                ["last_update"] = DateTime.UtcNow.ToString("o"),
            };
            UpsertStation(p, true);
        }

        public void EmitMessageUpdate(string message)
        {
            _lastMessage = message ?? ""; _haveMessage = true;
            var p = new JObject { ["message"] = _lastMessage };
            FireAndForget("message_update", p);
        }

        private void FireAndForget(string evtName, JToken payload)
        {
            try
            {
                /* Centralised report/view policy: a "view" client is
                 * read-only.  We still let Emit* methods update their
                 * cached state (_lastFreqHz / _lastMessage / etc.) so a
                 * later role flip to "report" can re-publish the
                 * accumulated state via ConnectAndPumpAsync step 4 -- we
                 * just suppress the wire send here. */
                if (string.Equals(Role, "view", StringComparison.Ordinal)) return;
                if (_ws == null || _ws.State != WebSocketState.Open) return;
                var arr = new JArray { evtName };
                if (payload != null) arr.Add(payload);
                var frame = "42" + arr.ToString(Formatting.None);
                _ = SendRawAsync(frame, _cts != null ? _cts.Token : CancellationToken.None);
            }
            catch (Exception ex) { Log("emit " + evtName + " failed: " + ex.Message); }
        }

        private async Task RunLoop(CancellationToken ct)
        {
            Log("RunLoop entry");
            while (!ct.IsCancellationRequested)
            {
                try { await ConnectAndPumpAsync(ct); }
                catch (OperationCanceledException) { break; }
                catch (Exception ex)
                {
                    Log("session ended: " + ex.GetType().Name + " " + ex.Message);
                    if (ex.InnerException != null)
                        Log("  inner: " + ex.InnerException.GetType().Name + " " + ex.InnerException.Message);

                    // [Reporter logging] Communication lost — surface to NetErrorLog with reason.
                    string innerMsg = ex.InnerException == null
                                      ? ""
                                      : " (inner: " + ex.InnerException.GetType().Name + " " + ex.InnerException.Message + ")";
                    Common.LogReporter("Communication lost: "
                        + ex.GetType().Name + " " + ex.Message + innerMsg
                        + " — reconnecting in 5s");
                }
                SetState("Reconnecting in 5s");
                try { await Task.Delay(5000, ct); }
                catch (OperationCanceledException) { break; }
            }
            Log("RunLoop exit");
            SetState("Disconnected");
            Common.LogReporter("Reporter exit — client disconnected");
        }

        private async Task ConnectAndPumpAsync(CancellationToken ct)
        {
            SetState("Connecting");
            _ws = new ClientWebSocket();
            /* NOTE: do NOT call _ws.Options.SetRequestHeader("User-Agent", ...).
             * .NET Framework's HttpWebRequest treats User-Agent as a
             * restricted header and SetRequestHeader throws ArgumentException
             * for it -- ClientWebSocket exposes no alternate setter.  We
             * leave the default UA which the server accepts. */

            string host = Hostname; int port = UseTls ? 443 : 80;
            int colon = host.IndexOf(':');
            if (colon > 0 && int.TryParse(host.Substring(colon + 1), out int p))
            {
                port = p; host = host.Substring(0, colon);
            }
            string scheme = UseTls ? "wss" : "ws";
            var uri = new Uri(scheme + "://" + host + ":" + port +
                              "/socket.io/?EIO=4&transport=websocket");
            Log("WS connecting to " + uri);

            await _ws.ConnectAsync(uri, ct).ConfigureAwait(false);
            Log("WS connected");

            /* 1) Engine.IO open packet from server.  Format: "0{json}". */
            string open = await ReceivePacketAsync(ct).ConfigureAwait(false);
            Log("EIO open packet: " + (open ?? "<null>"));
            if (open == null || !open.StartsWith("0"))
                throw new Exception("expected EIO open, got: " + (open ?? "<null>"));
            var openObj = JObject.Parse(open.Substring(1));
            _pingIntervalMs = openObj.Value<int?>("pingInterval") ?? 25000;
            _pingTimeoutMs  = openObj.Value<int?>("pingTimeout")  ?? 20000;
            Log("ping interval=" + _pingIntervalMs + "ms timeout=" + _pingTimeoutMs + "ms");

            /* 2) Socket.IO connect with auth payload.  Format: "40{json}". */
            var auth = new JObject { ["protocol_version"] = PROTOCOL_VERSION };
            string roleEffective = Role;
            if (string.IsNullOrEmpty(Callsign) || string.IsNullOrEmpty(GridSquare))
                roleEffective = "view";
            auth["role"] = roleEffective;
            if (roleEffective != "view")
            {
                auth["callsign"]    = Callsign ?? "";
                auth["grid_square"] = GridSquare ?? "";
                auth["version"]     = ClientName ?? "Thetis";
                auth["rx_only"]     = RxOnly;
                auth["os"]          = Os ?? "Windows";
            }
            string authStr = "40" + auth.ToString(Formatting.None);
            Log("sending CONNECT: " + authStr);

            // [Reporter logging] Login attempt — capture host + credentials the user is trying.
            Common.LogReporter("Login attempt: host=" + host + ":" + port
                + " role=" + roleEffective
                + " call=" + (string.IsNullOrEmpty(Callsign) ? "<empty>" : Callsign)
                + " grid=" + (string.IsNullOrEmpty(GridSquare) ? "<empty>" : GridSquare)
                + " client=" + (ClientName ?? "Thetis")
                + " rx_only=" + RxOnly);

            await SendRawAsync(authStr, ct).ConfigureAwait(false);

            /* 3) Wait for Socket.IO connect ack ("40{...}") or error ("44{...}").
             *    The server sends an Engine.IO PING ("2") right after the
             *    OPEN packet -- often *before* our connect ack lands -- so
             *    consume any number of pings (responding with PONG) and
             *    other non-CONNECT packets while we wait. */
            string ack = null;
            while (true)
            {
                string pkt = await ReceivePacketAsync(ct).ConfigureAwait(false);
                if (pkt == null) throw new Exception("server closed before ack");
                Log("pre-ack rx: " + (pkt.Length > 200 ? pkt.Substring(0, 200) + "..." : pkt));
                if (pkt == "2") { await SendRawAsync("3", ct).ConfigureAwait(false); continue; }
                if (pkt.StartsWith("44"))
                {
                    // [Reporter logging] Server rejected our connect — log fail reason.
                    Common.LogReporter("Connection FAILED: server rejected connect: "
                        + pkt.Substring(2));
                    throw new Exception("server rejected connect: " + pkt.Substring(2));
                }
                if (pkt.StartsWith("40")) { ack = pkt; break; }
                /* Anything else (early events, ignored). */
            }
            Log("got CONNECT-ack: " + ack);

            // [Reporter logging] Connection successful.
            Common.LogReporter("Connected successfully: host=" + host + ":" + port
                + " role=" + roleEffective);

            /* Parse our own sid from the ack so LocalMirror* methods can
             * route self-emits into the Stations dict. */
            try
            {
                if (ack.Length > 2)
                {
                    var ackObj = JObject.Parse(ack.Substring(2));
                    MySid = ackObj["sid"]?.ToString();
                    Log("MySid = " + (MySid ?? "<null>"));
                }
            }
            catch (Exception ex) { Log("ack parse: " + ex.Message); }

            SetState("Connected (" + roleEffective + ")");
            Stations.Clear();
            StationsChanged?.Invoke(this, EventArgs.Empty);

            /* 4) Re-publish our last-known outbound state so the server's
             *    view stays consistent across drops. */
            if (roleEffective == "report")
            {
                if (_haveFreq)    FireAndForget("freq_change",   new JObject { ["freq"] = _lastFreqHz });
                if (_haveTxState) FireAndForget("tx_report",     new JObject { ["mode"] = _lastMode, ["transmitting"] = _lastTransmitting });
                if (_haveMessage) FireAndForget("message_update", new JObject { ["message"] = _lastMessage });
            }

            /* 5) Receive loop.  Engine.IO pings (server-driven, packet "2")
             *    are answered with pong ("3"). */
            while (_ws.State == WebSocketState.Open && !ct.IsCancellationRequested)
            {
                string pkt = await ReceivePacketAsync(ct).ConfigureAwait(false);
                if (pkt == null) break;
                if (pkt == "2")
                {
                    await SendRawAsync("3", ct).ConfigureAwait(false);
                    continue;
                }
                HandlePacket(pkt);
            }
        }

        private void HandlePacket(string packet)
        {
            if (packet.Length == 0) return;
            char type = packet[0];
            if (type == '4' && packet.Length >= 2)
            {
                char sub = packet[1];
                if (sub == '0')
                {
                    /* Repeated namespace connect ack -- ignore. */
                    return;
                }
                if (sub == '2')
                {
                    /* Socket.IO event: 42[name, payload, ...]. */
                    try
                    {
                        var arr = JArray.Parse(packet.Substring(2));
                        if (arr.Count >= 1)
                        {
                            string name = arr[0].ToString();
                            JToken payload = arr.Count >= 2 ? arr[1] : null;
                            HandleEvent(name, payload);
                        }
                    }
                    catch (Exception ex) { Log("event parse failed: " + ex.Message + " on: " + packet); }
                    return;
                }
                if (sub == '4')
                {
                    Log("server error: " + packet.Substring(2));
                    return;
                }
                if (sub == '1')
                {
                    Log("server disconnect");
                    return;
                }
            }
            /* Other packet types ignored (binary events, acks, upgrades). */
        }

        private void HandleEvent(string name, JToken payload)
        {
            switch (name)
            {
                case "new_connection":
                case "freq_change":
                case "tx_report":
                case "message_update":
                    UpsertStation(payload, false);
                    break;
                case "rx_report":
                    UpsertStation(payload, true);
                    break;
                case "remove_connection":
                {
                    var sid = payload?["sid"]?.ToString();
                    if (!string.IsNullOrEmpty(sid))
                        Stations.TryRemove(sid, out _);
                    StationsChanged?.Invoke(this, EventArgs.Empty);
                    break;
                }
                case "bulk_update":
                {
                    if (payload is JArray bulk)
                    {
                        foreach (var item in bulk)
                            if (item is JArray pair && pair.Count >= 2)
                                HandleEvent(pair[0].ToString(), pair[1]);
                        StationsChanged?.Invoke(this, EventArgs.Empty);
                    }
                    break;
                }
                case "connection_successful":
                    Log("connection_successful");
                    break;
                case "qsy_request":
                    /* Inbound QSY request -- we do not act on it from here;
                     * UI surface for QSY is a future add. */
                    break;
            }
        }

        private void UpsertStation(JToken p, bool isRxReport)
        {
            if (p == null || p.Type != JTokenType.Object) return;
            var sid = p["sid"]?.ToString();
            if (string.IsNullOrEmpty(sid)) return;

            var st = Stations.GetOrAdd(sid, _ => new StationInfo { Sid = sid });

            if (p["callsign"]      != null) { if (isRxReport) st.LastRxCallsign = p["callsign"].ToString(); else st.Callsign = p["callsign"].ToString(); }
            if (p["grid_square"]   != null) {
                /* Some stations advertise 8+ char (subsubsquare) locators.
                 * Maidenhead.cs accepts only 4 or 6 char input, and the UI
                 * is configured to show at most 6 -- truncate at the source
                 * so display and distance/heading calculations agree. */
                string g = p["grid_square"].ToString() ?? "";
                if (g.Length > 6) g = g.Substring(0, 6);
                st.GridSquare = g;
            }
            if (p["version"]       != null) st.Version    = p["version"].ToString();
            if (p["rx_only"]       != null) st.RxOnly     = p["rx_only"].Type == JTokenType.Boolean && (bool)p["rx_only"];
            if (p["freq"]          != null) st.FrequencyHz = (ulong)p["freq"].Value<long>();
            if (p["mode"]          != null) { if (isRxReport) st.LastRxMode = p["mode"].ToString(); else st.Mode = p["mode"].ToString(); }
            if (p["transmitting"]  != null) {
                st.Transmitting = p["transmitting"].Type == JTokenType.Boolean && (bool)p["transmitting"];
                if (st.Transmitting) st.LastTxActiveUtc = DateTime.UtcNow;
            }
            if (p["message"]       != null) {
                string newMsg = p["message"].ToString() ?? "";
                if (!string.Equals(newMsg, st.Message, StringComparison.Ordinal))
                    st.LastMessageChangeUtc = DateTime.UtcNow;
                st.Message = newMsg;
            }
            if (p["snr"]           != null && isRxReport)
            {
                try { st.LastRxSnr = p["snr"].Value<double>(); } catch { }
                st.LastRxUtc = DateTime.UtcNow;
            }
            DateTime parsed;
            if (TryParseIso(p["last_update"]?.ToString(),  out parsed)) st.LastUpdateUtc = parsed;
            if (TryParseIso(p["connect_time"]?.ToString(), out parsed)) st.ConnectTimeUtc = parsed;
            if (TryParseIso(p["last_tx"]?.ToString(),      out parsed)) st.LastTxUtc = parsed;

            StationsChanged?.Invoke(this, EventArgs.Empty);
        }

        private static bool TryParseIso(string s, out DateTime dt)
        {
            if (!string.IsNullOrEmpty(s) &&
                DateTime.TryParse(s, System.Globalization.CultureInfo.InvariantCulture,
                                  System.Globalization.DateTimeStyles.AssumeUniversal |
                                  System.Globalization.DateTimeStyles.AdjustToUniversal,
                                  out dt))
                return true;
            dt = DateTime.MinValue; return false;
        }

        /* ============================================================
         *  Transport helpers
         * ============================================================ */

        private async Task SendRawAsync(string frame, CancellationToken ct)
        {
            await _sendLock.WaitAsync(ct).ConfigureAwait(false);
            try
            {
                if (_ws == null || _ws.State != WebSocketState.Open) return;
                var bytes = Encoding.UTF8.GetBytes(frame);
                await _ws.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, true, ct)
                         .ConfigureAwait(false);
            }
            finally { try { _sendLock.Release(); } catch { } }
        }

        private async Task<string> ReceivePacketAsync(CancellationToken ct)
        {
            var buffer = new byte[16 * 1024];
            using (var ms = new System.IO.MemoryStream())
            {
                while (true)
                {
                    WebSocketReceiveResult r;
                    try
                    {
                        r = await _ws.ReceiveAsync(new ArraySegment<byte>(buffer), ct)
                                     .ConfigureAwait(false);
                    }
                    catch { return null; }
                    if (r.MessageType == WebSocketMessageType.Close) return null;
                    ms.Write(buffer, 0, r.Count);
                    if (r.EndOfMessage) break;
                }
                return Encoding.UTF8.GetString(ms.ToArray());
            }
        }

        private void SetState(string s)
        {
            ConnectionState = s;
            ConnectionStateChanged?.Invoke(this, s);
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        private static extern void OutputDebugStringW([MarshalAs(UnmanagedType.LPWStr)] string lpOutputString);

        private void Log(string s)
        {
            /* DebugView (Sysinternals) listens to OutputDebugString.
             * Debug.WriteLine is no-op in Release builds, so we go via
             * the kernel32 export directly.  Always emit, never throw. */
            try { OutputDebugStringW("[FreeDVReporter] " + s + "\n"); } catch { }
            LogMessage?.Invoke(this, s);
        }
    }
}
