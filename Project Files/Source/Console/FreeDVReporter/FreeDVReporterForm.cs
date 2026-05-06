/*  FreeDVReporterForm.cs
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
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace Thetis.FreeDVReporter
{
    public class FreeDVReporterForm : Form
    {
        private readonly FreeDVReporterClient _client;

        /* UI elements */
        private MenuStrip menu;
        private ToolStripMenuItem mnuShow;
        private ToolStripMenuItem mnuFilter;
        private ToolStrip toolbar;
        private ToolStripLabel lblBand;
        private ToolStripComboBox cmbBand;
        private ToolStripLabel lblTrack;
        private ToolStripButton btnTrack;
        private DataGridView grid;
        private StatusStrip status;
        private ToolStripStatusLabel lblConn;
        private ToolStripStatusLabel lblFilterStatus;
        private ToolStripStatusLabel lblCount;
        private System.Windows.Forms.Timer refreshTimer;

        /* sid -> row, so per-tick updates only touch changed cells. */
        private readonly Dictionary<string, DataGridViewRow> _rowBySid =
            new Dictionary<string, DataGridViewRow>(StringComparer.Ordinal);

        /* The selection visual is suppressed entirely -- per-row
         * SelectionBackColor is painted to match the row's TX/RX shade,
         * so a "selected" row looks identical to an unselected one.
         * No timer / deselect logic needed. */

        /* Sentinel for "unknown" in numeric columns -- formatted as ""
         * by CellFormatting, sorts to the bottom in ascending order so
         * unknowns don't pollute the top of a "closest first" list. */
        private const double UNKNOWN_NUMBER = double.MaxValue;

        /* SyncRows triggers many internal SelectionChanged events --
         *   - DataGridView.Rows.Add() auto-sets CurrentCell when the grid
         *     was empty, which lights up the new row.
         *   - row.Visible = false/true (band + idle filters) re-points
         *     CurrentCell to the next visible row.
         *   - Rows.Remove(selected row) re-points CurrentCell to a sibling.
         *   - grid.Sort() may shuffle CurrentCell.
         * Any of these would otherwise stamp _selectionAtUtc = now and
         * the user would see an unprovoked blue flash that the deselect
         * timer clears 5 s later, on every tick.  Set the flag for the
         * whole tick; user clicks happen between ticks. */
        private bool _suppressSelectionEvents;

        /* Column index constants -- keep in sync with BuildColumns(). */
        private const int COL_CALL = 0;
        private const int COL_GRID = 1;
        private const int COL_KM   = 2;
        private const int COL_HDG  = 3;
        private const int COL_VER  = 4;
        private const int COL_KHZ  = 5;
        private const int COL_MODE = 6;
        private const int COL_STAT = 7;
        private const int COL_MSG  = 8;
        private const int COL_LTX  = 9;
        private const int COL_RXC  = 10;
        private const int COL_RXM  = 11;
        private const int COL_SNR  = 12;
        private const int COL_UPD  = 13;

        private static readonly string[] COL_HEADERS =
        {
            "Callsign", "Locator", "km", "Hdg", "Version",
            "kHz", "Mode", "Status", "Msg", "Last TX",
            "RX Call", "RX Mode", "SNR", "Updated"
        };

        /* Static so column visibility, band selection, idle filter etc.
         * persist while the app is running -- the form may be closed and
         * re-opened by the chkRADAEReporter checkbox. */
        private static readonly bool[] _colVisible = InitColVisible();
        private static HamBand _bandFilter = HamBand.All;       /* default: All */
        private static bool    _trackBand  = false;             /* default: don't track radio */
        private static int     _idleMinutes = 0;                /* 0 = disabled */

        /* Window pos + size persisted via Common.SaveForm/RestoreForm
         * (writes to DB.SaveVars under "FreeDVReporterForm" tablename --
         * same pattern as AmpView, CWX, BandStack2, EQForm).  No
         * in-process state needed -- the DB layer survives form
         * destruction and app restart. */

        private static bool[] InitColVisible()
        {
            var v = new bool[COL_HEADERS.Length];
            for (int i = 0; i < v.Length; i++) v[i] = true;
            v[COL_RXM] = false;     /* hidden by default, matches FreeDV-GUI */
            return v;
        }

        public Action OnUserClose;

        public FreeDVReporterForm(FreeDVReporterClient client)
        {
            _client = client;
            BuildUi();
            ApplyColumnVisibility();
            ApplyBandToCombo();

            _client.StationsChanged        += OnStationsChanged;
            _client.ConnectionStateChanged += OnConnStateChanged;

            FormClosing += (s, e) =>
            {
                if (e.CloseReason == CloseReason.UserClosing && OnUserClose != null)
                {
                    e.Cancel = true;
                    Hide();
                    OnUserClose();
                }
            };
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                try
                {
                    _client.StationsChanged        -= OnStationsChanged;
                    _client.ConnectionStateChanged -= OnConnStateChanged;
                    refreshTimer?.Stop();
                }
                catch { }
            }
            base.Dispose(disposing);
        }

        /* ============================================================
         *  UI construction
         * ============================================================ */

        private void BuildUi()
        {
            Text = "FreeDV Reporter";
            MinimumSize = new Size(720, 280);
            ShowInTaskbar = true;          /* match FreeDV-GUI: separate taskbar entry */
            KeyPreview = true;

            /* Initial fallback geometry -- Common.RestoreForm at the end
             * of BuildUi will overwrite Top/Left/Width/Height (and switch
             * StartPosition to Manual) if a saved record exists. */
            StartPosition = FormStartPosition.CenterScreen;
            Size = new Size(1100, 500);

            /* Persist geometry on close.  Common.SaveForm writes
             * Top/Left/Width/Height to DB.SaveVars under
             * "FreeDVReporterForm" -- restored on the next form
             * construction.  Pattern matches AmpView, CWX,
             * BandStack2, EQForm. */
            FormClosing += (s, e) =>
            {
                try { Thetis.Common.SaveForm(this, "FreeDVReporterForm"); }
                catch { }
            };

            BuildMenu();
            BuildToolbar();
            BuildGrid();
            BuildStatusBar();

            /* Order matters for Dock fill: status (Bottom), toolbar (Top after menu),
             * menu (Top), grid (Fill). */
            Controls.Add(grid);
            Controls.Add(toolbar);
            Controls.Add(status);
            Controls.Add(menu);
            MainMenuStrip = menu;

            refreshTimer = new System.Windows.Forms.Timer { Interval = 1000 };
            refreshTimer.Tick += (s, e) => SyncRows();
            refreshTimer.Start();

            /* Restore last saved geometry from the DB.  RestoreForm
             * flips StartPosition to Manual when it finds a record
             * (and writes Top/Left/Width/Height directly), so this
             * call must come after the controls have been added so
             * MinimumSize / layout is settled. */
            try { Thetis.Common.RestoreForm(this, "FreeDVReporterForm", true); }
            catch { }
        }

        private void BuildMenu()
        {
            menu = new MenuStrip();

            /* Show menu -- one toggle per column */
            mnuShow = new ToolStripMenuItem("Show");
            for (int i = 0; i < COL_HEADERS.Length; i++)
            {
                int idx = i;       /* capture */
                var item = new ToolStripMenuItem(COL_HEADERS[i]) { CheckOnClick = true, Checked = _colVisible[i] };
                item.CheckedChanged += (s, e) =>
                {
                    _colVisible[idx] = item.Checked;
                    if (grid != null && idx < grid.Columns.Count)
                        grid.Columns[idx].Visible = item.Checked;
                };
                mnuShow.DropDownItems.Add(item);
            }
            menu.Items.Add(mnuShow);

            /* Filter -> Idle more than (minutes) submenu */
            mnuFilter = new ToolStripMenuItem("Filter");
            var idleRoot = new ToolStripMenuItem("Idle more than (minutes)");
            int[] presets = { 0, 30, 60, 90, 120 };
            foreach (var p in presets)
            {
                int local = p;
                string label = local == 0 ? "Disabled" : local.ToString();
                var it = new ToolStripMenuItem(label) { CheckOnClick = false, Checked = _idleMinutes == local };
                it.Click += (s, e) => { _idleMinutes = local; UpdateIdleChecks(idleRoot); UpdateFilterStatusLabel(); };
                idleRoot.DropDownItems.Add(it);
            }
            var custom = new ToolStripMenuItem("Custom...");
            custom.Click += (s, e) =>
            {
                using (var dlg = new IdleCustomDialog(_idleMinutes))
                {
                    if (dlg.ShowDialog(this) == DialogResult.OK)
                    {
                        _idleMinutes = dlg.Minutes;
                        UpdateIdleChecks(idleRoot);
                        UpdateFilterStatusLabel();
                    }
                }
            };
            idleRoot.DropDownItems.Add(custom);
            mnuFilter.DropDownItems.Add(idleRoot);
            menu.Items.Add(mnuFilter);
        }

        private static void UpdateIdleChecks(ToolStripMenuItem idleRoot)
        {
            int[] presets = { 0, 30, 60, 90, 120 };
            for (int i = 0; i < presets.Length; i++)
            {
                if (i < idleRoot.DropDownItems.Count && idleRoot.DropDownItems[i] is ToolStripMenuItem it)
                    it.Checked = (presets[i] == _idleMinutes);
            }
            /* Custom item is the last one -- mark it when no preset matches */
            int custIdx = idleRoot.DropDownItems.Count - 1;
            if (idleRoot.DropDownItems[custIdx] is ToolStripMenuItem cm)
                cm.Checked = (_idleMinutes != 0 && Array.IndexOf(presets, _idleMinutes) < 0);
        }

        private void BuildToolbar()
        {
            toolbar = new ToolStrip { GripStyle = ToolStripGripStyle.Hidden };

            lblBand = new ToolStripLabel("Band:");
            cmbBand = new ToolStripComboBox { DropDownStyle = ComboBoxStyle.DropDownList, AutoSize = false, Width = 100 };
            foreach (HamBand b in Enum.GetValues(typeof(HamBand)))
                cmbBand.Items.Add(BandPlan.Label(b));
            cmbBand.SelectedIndexChanged += (s, e) =>
            {
                if (cmbBand.SelectedIndex < 0) return;
                _bandFilter = (HamBand)cmbBand.SelectedIndex;
            };

            btnTrack = new ToolStripButton("Track radio") { CheckOnClick = true, Checked = _trackBand };
            btnTrack.CheckedChanged += (s, e) =>
            {
                _trackBand = btnTrack.Checked;
                cmbBand.Enabled = !_trackBand;
            };
            cmbBand.Enabled = !_trackBand;

            toolbar.Items.Add(lblBand);
            toolbar.Items.Add(cmbBand);
            toolbar.Items.Add(new ToolStripSeparator());
            toolbar.Items.Add(btnTrack);
        }

        private void BuildGrid()
        {
            grid = new DataGridView
            {
                Dock = DockStyle.Fill,
                AllowUserToAddRows = false,
                AllowUserToDeleteRows = false,
                AllowUserToResizeRows = false,
                AllowUserToOrderColumns = true,
                ReadOnly = true,
                RowHeadersVisible = false,
                SelectionMode = DataGridViewSelectionMode.FullRowSelect,
                AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.None,
                BackgroundColor = SystemColors.Window,
                MultiSelect = false,
            };
            BuildColumns();

            /* Render the UNKNOWN_NUMBER sentinel as blank so the sort works
             * but the user sees an empty cell. */
            grid.CellFormatting += (s, e) =>
            {
                if (e.Value is double d && d >= UNKNOWN_NUMBER)
                {
                    e.Value = "";
                    e.FormattingApplied = true;
                }
            };

            /* Double-click a station row -> tune VFOA to that station's
             * reported frequency and snap the operating mode to DIGL
             * (<12 MHz) or DIGU (>=12 MHz).  The sid is stashed on each
             * row's Tag at creation. */
            grid.CellDoubleClick += (s, e) =>
            {
                if (e.RowIndex < 0 || e.RowIndex >= grid.Rows.Count) return;
                string sid = grid.Rows[e.RowIndex].Tag as string;
                if (string.IsNullOrEmpty(sid)) return;
                StationInfo st;
                if (!_client.Stations.TryGetValue(sid, out st)) return;
                if (st.FrequencyHz == 0) return;
                FreeDVReporterManager.TuneToFrequency(st.FrequencyHz);
            };
        }

        private void BuildColumns()
        {
            int[] widths = { 90, 70, 60, 50, 90, 90, 60, 70, 220, 80, 80, 70, 50, 80 };
            for (int i = 0; i < COL_HEADERS.Length; i++)
            {
                bool numeric = (i == COL_KM || i == COL_HDG || i == COL_KHZ || i == COL_SNR);
                var col = new DataGridViewTextBoxColumn
                {
                    HeaderText = COL_HEADERS[i],
                    Name = "col" + i,
                    Width = widths[i],
                    SortMode = DataGridViewColumnSortMode.Automatic,
                    DefaultCellStyle = new DataGridViewCellStyle
                    {
                        Alignment = numeric ? DataGridViewContentAlignment.MiddleRight
                                            : DataGridViewContentAlignment.MiddleLeft,
                    },
                };
                /* Use Numeric value type for the columns we want to sort numerically. */
                if (numeric) col.ValueType = typeof(double);
                grid.Columns.Add(col);
            }
            /* Default sort: km ascending (closest stations first). */
            grid.Sort(grid.Columns[COL_KM], System.ComponentModel.ListSortDirection.Ascending);
        }

        private void ApplyColumnVisibility()
        {
            for (int i = 0; i < grid.Columns.Count && i < _colVisible.Length; i++)
                grid.Columns[i].Visible = _colVisible[i];
        }

        private void ApplyBandToCombo()
        {
            int idx = (int)_bandFilter;
            if (idx >= 0 && idx < cmbBand.Items.Count) cmbBand.SelectedIndex = idx;
        }

        private void BuildStatusBar()
        {
            status = new StatusStrip { SizingGrip = false };
            lblConn         = new ToolStripStatusLabel("Connection: Disconnected") { Spring = true, TextAlign = ContentAlignment.MiddleLeft };
            lblFilterStatus = new ToolStripStatusLabel("Idle Off");
            lblCount        = new ToolStripStatusLabel("0 stations");
            status.Items.Add(lblConn);
            status.Items.Add(lblFilterStatus);
            status.Items.Add(new ToolStripSeparator());
            status.Items.Add(lblCount);
            UpdateFilterStatusLabel();
        }

        private void UpdateFilterStatusLabel()
        {
            if (lblFilterStatus == null) return;
            lblFilterStatus.Text = _idleMinutes == 0 ? "Idle Off" : ("Idle >" + _idleMinutes + "m");
        }

        /* ============================================================
         *  Event hookup helpers (marshal to UI thread)
         * ============================================================ */

        private void OnStationsChanged(object sender, EventArgs e)
        {
            /* No-op: the 1-Hz timer drives sync to coalesce bursts. */
        }

        private void OnConnStateChanged(object sender, string state)
        {
            if (IsDisposed || !IsHandleCreated) return;
            try { BeginInvoke(new Action(() => { lblConn.Text = "Connection: " + state; })); } catch { }
        }

        /* ============================================================
         *  Per-tick incremental sync.  Keeps grid sort + selection.
         * ============================================================ */

        private void SyncRows()
        {
            if (grid == null || IsDisposed || !IsHandleCreated) return;

            /* Suppress for the entire tick.  Add/Remove/Visible/Sort all
             * fire SelectionChanged internally; without this gate, every
             * tick stamps _selectionAtUtc and produces an unprovoked
             * blue flash.  User clicks happen between ticks (UI thread
             * is single-threaded), so wrapping the whole tick is safe. */
            _suppressSelectionEvents = true;
            try { SyncRowsInner(); }
            finally { _suppressSelectionEvents = false; }
        }

        private void SyncRowsInner()
        {

            /* "Track radio" mode: pull current band from the manager's
             * last-pushed VFOA freq.  No-op if 0. */
            if (_trackBand)
            {
                ulong f = FreeDVReporterManager.CurrentFrequencyHz;
                if (f > 0)
                {
                    HamBand b = BandPlan.FromHz(f);
                    if (b != _bandFilter)
                    {
                        _bandFilter = b;
                        ApplyBandToCombo();
                    }
                }
            }

            string myGrid = FmtLocator(_client.GridSquare ?? "");

            /* Snapshot keys we still see this tick. */
            var seen = new HashSet<string>(StringComparer.Ordinal);
            DateTime nowUtc = DateTime.UtcNow;
            int idleCutSec = _idleMinutes * 60;

            grid.SuspendLayout();
            try
            {
                foreach (var kv in _client.Stations)
                {
                    var sid = kv.Key;
                    var st  = kv.Value;
                    seen.Add(sid);

                    DataGridViewRow row;
                    if (!_rowBySid.TryGetValue(sid, out row))
                    {
                        int idx = grid.Rows.Add();
                        row = grid.Rows[idx];
                        _rowBySid[sid] = row;
                        /* Stash sid on Tag so the CellDoubleClick handler
                         * can recover the station identity from a click. */
                        row.Tag = sid;
                        /* Suppress the system-blue selection highlight from
                         * the moment the row exists -- shade-update block
                         * below will repaint to TX/RX/MSG colour as needed. */
                        row.DefaultCellStyle.SelectionBackColor = SystemColors.Window;
                        row.DefaultCellStyle.SelectionForeColor = SystemColors.ControlText;
                    }

                    bool inBand = _bandFilter == HamBand.All ||
                                  (st.FrequencyHz > 0 && BandPlan.FromHz(st.FrequencyHz) == _bandFilter);
                    bool tooIdle = _idleMinutes > 0 && st.LastUpdateUtc != DateTime.MinValue &&
                                   (nowUtc - st.LastUpdateUtc).TotalSeconds > idleCutSec;
                    bool visible = inBand && !tooIdle;

                    if (row.Visible != visible) row.Visible = visible;
                    if (!visible) continue;

                    string callDisp = FmtCallsign(st.Callsign);
                    string gridDisp = FmtLocator(st.GridSquare);

                    double? km = null, hdg = null;
                    if (!string.IsNullOrEmpty(myGrid) && !string.IsNullOrEmpty(gridDisp))
                    {
                        double d = Maidenhead.DistanceKm(myGrid, gridDisp);
                        if (!double.IsNaN(d)) km = d;
                        double b = Maidenhead.BearingDeg(myGrid, gridDisp);
                        if (!double.IsNaN(b)) hdg = b;
                    }

                    SetCell(row, COL_CALL, callDisp);
                    SetCell(row, COL_GRID, gridDisp);
                    /* Numeric columns: store a real double so DataGridView's
                     * default sort works.  UNKNOWN_NUMBER is the sentinel
                     * for "no value" -- CellFormatting renders it blank. */
                    SetCell(row, COL_KM,  km.HasValue  ? Math.Round(km.Value)  : UNKNOWN_NUMBER);
                    SetCell(row, COL_HDG, hdg.HasValue ? Math.Round(hdg.Value) : UNKNOWN_NUMBER);
                    SetCell(row, COL_VER, st.Version);
                    SetCell(row, COL_KHZ, st.FrequencyHz > 0 ? (st.FrequencyHz / 1000.0) : UNKNOWN_NUMBER);
                    SetCell(row, COL_MODE, st.Mode);
                    SetCell(row, COL_STAT, st.RxOnly ? "RX Only" : (st.Transmitting ? "TX" : ""));
                    SetCell(row, COL_MSG,  st.Message);
                    SetCell(row, COL_LTX,  FmtTime(st.LastTxUtc));
                    SetCell(row, COL_RXC,  FmtCallsign(st.LastRxCallsign));
                    SetCell(row, COL_RXM,  st.LastRxMode);
                    SetCell(row, COL_SNR, double.IsNaN(st.LastRxSnr) ? UNKNOWN_NUMBER : st.LastRxSnr);
                    SetCell(row, COL_UPD,  FmtTime(st.LastUpdateUtc == DateTime.MinValue ? (DateTime?)null : st.LastUpdateUtc));

                    /* Row-shade priority (matches FreeDV-GUI):
                     *   Msg just changed      -> Plum       (5 s tail; highest)
                     *   Recently transmitting -> LightCoral (5 s tail after TX stops)
                     *   Recently received     -> LimeGreen  (5 s tail after rx_report)
                     *   Otherwise             -> default                                 */
                    Color want = SystemColors.Window;
                    bool recentMsg = st.LastMessageChangeUtc.HasValue &&
                                     (nowUtc - st.LastMessageChangeUtc.Value).TotalSeconds < 5.0;
                    bool recentTx  = st.Transmitting ||
                                     (st.LastTxActiveUtc.HasValue &&
                                      (nowUtc - st.LastTxActiveUtc.Value).TotalSeconds < 5.0);
                    bool recentRx  = st.LastRxUtc.HasValue &&
                                     (nowUtc - st.LastRxUtc.Value).TotalSeconds < 5.0;
                    if      (recentMsg) want = Color.Plum;
                    else if (recentTx)  want = Color.LightCoral;
                    else if (recentRx)  want = Color.LimeGreen;
                    if (row.DefaultCellStyle.BackColor != want)
                    {
                        row.DefaultCellStyle.BackColor = want;
                        /* Mirror SelectionBackColor onto the same shade so
                         * a "selected" row never shows the system-blue
                         * highlight -- the selection visual is suppressed
                         * entirely.  ForeColor stays default for contrast. */
                        row.DefaultCellStyle.SelectionBackColor = want;
                        row.DefaultCellStyle.SelectionForeColor = SystemColors.ControlText;
                    }
                }

                /* Drop rows whose sid disappeared. */
                if (_rowBySid.Count > seen.Count)
                {
                    var stale = _rowBySid.Where(kv => !seen.Contains(kv.Key)).Select(kv => kv.Key).ToList();
                    foreach (var sid in stale)
                    {
                        var row = _rowBySid[sid];
                        _rowBySid.Remove(sid);
                        if (!row.IsNewRow) grid.Rows.Remove(row);
                    }
                }

                /* Re-apply the user's current sort.  DataGridView's Sort()
                 * is a one-shot in unbound mode -- new rows land at the
                 * bottom and existing rows whose key changed don't move
                 * unless we re-sort here. */
                if (grid.SortedColumn != null && grid.SortOrder != SortOrder.None)
                {
                    try
                    {
                        var dir = grid.SortOrder == SortOrder.Descending
                                  ? System.ComponentModel.ListSortDirection.Descending
                                  : System.ComponentModel.ListSortDirection.Ascending;
                        grid.Sort(grid.SortedColumn, dir);
                    }
                    catch { }
                }

                /* Visible row count for the status bar. */
                int n = 0;
                foreach (DataGridViewRow r in grid.Rows) if (r.Visible) n++;
                lblCount.Text = n + (n == 1 ? " station" : " stations");
            }
            finally { grid.ResumeLayout(); }
        }

        private static void SetCell(DataGridViewRow row, int col, object value)
        {
            var cell = row.Cells[col];
            if (!Equals(cell.Value, value)) cell.Value = value;
        }

        private static string FmtTime(DateTime? utc)
        {
            if (!utc.HasValue) return "";
            return utc.Value.ToLocalTime().ToString("HH:mm:ss");
        }

        /* Canonical Maidenhead locator presentation: field (chars 1-2)
         * upper, square (chars 3-4) digits, subsquare (chars 5-6) lower.
         * Anything shorter than 6 returned upper-cased.  This is also the
         * form fed to Maidenhead.DistanceKm/BearingDeg so display and
         * calculation always agree. */
        private static string FmtLocator(string g)
        {
            if (string.IsNullOrEmpty(g)) return "";
            string s = g.Trim();
            if (s.Length > 6) s = s.Substring(0, 6);
            if (s.Length <= 4) return s.ToUpperInvariant();
            return s.Substring(0, 4).ToUpperInvariant() + s.Substring(4).ToLowerInvariant();
        }

        private static string FmtCallsign(string c)
        {
            if (string.IsNullOrEmpty(c)) return "";
            return c.Trim().ToUpperInvariant();
        }

        /* ============================================================
         *  Custom-idle prompt
         * ============================================================ */

        private class IdleCustomDialog : Form
        {
            public int Minutes;
            private NumericUpDown nud;
            public IdleCustomDialog(int initial)
            {
                Text = "Custom idle threshold";
                FormBorderStyle = FormBorderStyle.FixedDialog;
                StartPosition = FormStartPosition.CenterParent;
                MinimizeBox = false; MaximizeBox = false;
                Size = new Size(300, 140);
                var lbl = new Label { Text = "Hide stations idle for more than (minutes):", Top = 14, Left = 12, AutoSize = true };
                nud = new NumericUpDown { Top = 38, Left = 12, Width = 100, Minimum = 1, Maximum = 1440, Value = Math.Max(1, initial == 0 ? 30 : initial) };
                var ok = new Button { Text = "OK", DialogResult = DialogResult.OK, Top = 70, Left = 110, Width = 70 };
                var cn = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Top = 70, Left = 190, Width = 70 };
                AcceptButton = ok; CancelButton = cn;
                Controls.Add(lbl); Controls.Add(nud); Controls.Add(ok); Controls.Add(cn);
                ok.Click += (s, e) => { Minutes = (int)nud.Value; };
            }
        }
    }
}
