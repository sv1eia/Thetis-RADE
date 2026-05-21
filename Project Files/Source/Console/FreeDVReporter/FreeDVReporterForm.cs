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
        private ToolStripButton btnTrackRx1;
        private ToolStripButton btnTrackRx2;
        private ToolStripButton btnTrackFreq;
        private ToolStripButton btnTrackBandMode;
        private Console.RX2EnabledChanged _rx2EnabledHandler;
        /* Console reference passed explicitly via the constructor.
         * Console.getConsole() is not safe at form-construction time
         * during initial startup because the Console ctor is still on
         * the stack; this field lets the form read the live console
         * state without the static-getter race. */
        private readonly Console _console;
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

        /* Sticky-top scroll behaviour.  When true, SyncRowsInner snaps
         * the visible top of the grid to the first visible row after
         * every refresh tick (so new stations sorting at the top stay
         * on screen, including on a small window).  Disengaged whenever
         * the user scrolls the grid away from the top; re-engaged when
         * they scroll back to the top.  Both transitions are detected
         * by inspecting the post-scroll FirstDisplayedScrollingRowIndex
         * in the grid.Scroll handler.  _suppressScrollEvent gates the
         * Scroll handler against our own programmatic scroll-to-top
         * inside SyncRowsInner -- otherwise that synthetic scroll would
         * be interpreted as a user gesture. */
        private bool _stickToTop = true;
        private bool _suppressScrollEvent = false;

        /* Column index constants -- keep in sync with BuildColumns(). */
        private const int COL_CALL = 0;
        private const int COL_GRID = 1;
        private const int COL_KM   = 2;
        private const int COL_HDG  = 3;
        private const int COL_VER  = 4;
        private const int COL_MHZ  = 5;
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
            "MHz", "Mode", "Status", "Msg", "Last TX",
            "RX Call", "RX Mode", "SNR", "Updated"
        };

        /* Static so column visibility, band selection, idle filter etc.
         * persist while the app is running -- the form may be closed and
         * re-opened by the chkRADAEReporter checkbox. */
        private static readonly bool[] _colVisible = InitColVisible();
        private static HamBand _bandFilter = HamBand.All;       /* default: All */
        private static int     _idleMinutes = 0;                /* 0 = disabled */

        /* Dual-RX: which receiver's frequency drives the band/frequency
         * filter.  Tri-state replacement for the legacy _trackBand bool --
         *   Off  = no tracking, cmbBand selection is honoured;
         *   Rx1  = track VFOA (the legacy behaviour);
         *   Rx2  = track VFOB.  btnTrackRx2 is greyed unless Console.RX2Enabled. */
        private enum TrackTarget { Off, Rx1, Rx2 }
        private static TrackTarget _trackTarget = TrackTarget.Off;

        /* When tracking is on, _trackMode picks how the tracked receiver's
         * frequency drives the row filter:
         *   Band      -- show stations on the same ham band (default).
         *   Frequency -- show only stations whose reported FrequencyHz
         *                is within +/- TRACK_FREQ_TOL_HZ of the tracked
         *                radio's frequency. */
        private enum TrackMode { Band, Frequency }
        private static TrackMode _trackMode = TrackMode.Band;
        private const long TRACK_FREQ_TOL_HZ = 100;             /* +/- 100 Hz */

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

        public FreeDVReporterForm(FreeDVReporterClient client, Console console)
        {
            _client = client;
            _console = console;
            BuildUi();
            ApplyColumnVisibility();
            ApplyBandToCombo();

            _client.StationsChanged        += OnStationsChanged;
            _client.ConnectionStateChanged += OnConnStateChanged;

            /* Subscribe to RX2 enable changes so the "Track RX2" button
             * follows the console chkRX2 state.  Uses the explicit _console
             * field (passed to the ctor) rather than Console.getConsole(),
             * which is unsafe during the very first startup -- see field
             * comment. */
            try
            {
                if (_console != null)
                {
                    _rx2EnabledHandler = (enabled) =>
                    {
                        if (IsDisposed) return;
                        if (InvokeRequired)
                        {
                            try { BeginInvoke(new Action(() => UpdateRx2Track(enabled))); } catch { }
                        }
                        else
                        {
                            UpdateRx2Track(enabled);
                        }
                    };
                    _console.RX2EnabledChangedHandlers += _rx2EnabledHandler;
                }
            }
            catch { }

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
                    if (_rx2EnabledHandler != null)
                    {
                        if (_console != null)
                            _console.RX2EnabledChangedHandlers -= _rx2EnabledHandler;
                        _rx2EnabledHandler = null;
                    }
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

            /* Dual-RX: "Track RX1" / "Track RX2" mutually-exclusive radio
             * buttons replace the legacy single "Track radio" toggle.
             * btnTrackRx2 is greyed unless the console reports RX2
             * enabled.  Click-the-active-button toggles tracking off
             * (parity with the legacy btnTrack toggle semantics). */
            bool rx2InitiallyEnabled = false;
            try { rx2InitiallyEnabled = _console?.RX2Enabled ?? false; } catch { }
            btnTrackRx1 = new ToolStripButton("Track RX1")
            {
                CheckOnClick = false,
                Checked = (_trackTarget == TrackTarget.Rx1),
            };
            btnTrackRx2 = new ToolStripButton("Track RX2")
            {
                CheckOnClick = false,
                Checked = (_trackTarget == TrackTarget.Rx2),
                Enabled = rx2InitiallyEnabled,
            };

            /* Sub-mode radio buttons -- only meaningful while a tracking
             * target is selected.  Use Click instead of CheckedChanged to
             * avoid feedback loops while we set Checked programmatically. */
            btnTrackFreq = new ToolStripButton("Frequency")
            {
                CheckOnClick = false,
                Checked = (_trackMode == TrackMode.Frequency),
                Enabled = (_trackTarget != TrackTarget.Off),
            };
            btnTrackBandMode = new ToolStripButton("Band")
            {
                CheckOnClick = false,
                Checked = (_trackMode == TrackMode.Band),
                Enabled = (_trackTarget != TrackTarget.Off),
            };
            btnTrackFreq.Click += (s, e) =>
            {
                if (_trackTarget == TrackTarget.Off) return;
                _trackMode = TrackMode.Frequency;
                btnTrackFreq.Checked = true;
                btnTrackBandMode.Checked = false;
            };
            btnTrackBandMode.Click += (s, e) =>
            {
                if (_trackTarget == TrackTarget.Off) return;
                _trackMode = TrackMode.Band;
                btnTrackBandMode.Checked = true;
                btnTrackFreq.Checked = false;
            };

            btnTrackRx1.Click += (s, e) =>
            {
                _trackTarget = (_trackTarget == TrackTarget.Rx1)
                               ? TrackTarget.Off
                               : TrackTarget.Rx1;
                SyncTrackButtons();
            };
            btnTrackRx2.Click += (s, e) =>
            {
                if (!btnTrackRx2.Enabled) return;
                _trackTarget = (_trackTarget == TrackTarget.Rx2)
                               ? TrackTarget.Off
                               : TrackTarget.Rx2;
                SyncTrackButtons();
            };
            SyncTrackButtons();   /* sets cmbBand.Enabled + sub-button enables */

            toolbar.Items.Add(lblBand);
            toolbar.Items.Add(cmbBand);
            toolbar.Items.Add(new ToolStripSeparator());
            toolbar.Items.Add(btnTrackRx1);
            toolbar.Items.Add(btnTrackRx2);
            toolbar.Items.Add(btnTrackFreq);
            toolbar.Items.Add(btnTrackBandMode);
        }

        /* Centralised visual + dependent-control sync for the dual-RX
         * track buttons.  Called whenever _trackTarget changes and from
         * UpdateRx2Track when the RX2 enable state flips. */
        private void SyncTrackButtons()
        {
            if (btnTrackRx1 == null || btnTrackRx2 == null) return;
            btnTrackRx1.Checked = (_trackTarget == TrackTarget.Rx1);
            btnTrackRx2.Checked = (_trackTarget == TrackTarget.Rx2);
            bool tracking = _trackTarget != TrackTarget.Off;
            if (cmbBand != null)            cmbBand.Enabled            = !tracking;
            if (btnTrackFreq != null)       btnTrackFreq.Enabled       = tracking;
            if (btnTrackBandMode != null)   btnTrackBandMode.Enabled   = tracking;
        }

        /* Called when console.RX2EnabledChangedHandlers fires.  When RX2
         * goes away while it was the tracking target, fall back silently
         * to Track RX1 so the user's "tracking is on" intent is preserved. */
        private void UpdateRx2Track(bool rx2Enabled)
        {
            if (btnTrackRx2 == null) return;
            btnTrackRx2.Enabled = rx2Enabled;
            if (!rx2Enabled && _trackTarget == TrackTarget.Rx2)
            {
                _trackTarget = TrackTarget.Rx1;
                SyncTrackButtons();
            }
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

            /* Sticky-top scroll tracking.  User scroll gestures (wheel,
             * scrollbar drag, keyboard) fire this handler with the new
             * FirstDisplayedScrollingRowIndex.  When the visible top is
             * row 0 (or no visible rows) we engage stickiness; when the
             * user has scrolled away from the top we disengage.  Our own
             * programmatic scroll-to-top inside SyncRowsInner sets
             * _suppressScrollEvent first, so it isn't mistaken for user
             * intent. */
            grid.Scroll += (s, e) =>
            {
                if (_suppressScrollEvent) return;
                try
                {
                    int idx = grid.FirstDisplayedScrollingRowIndex;
                    _stickToTop = (idx <= 0);
                }
                catch { _stickToTop = true; }
            };

            /* Right-click on the Msg cell -> "Copy Message" context menu.
             * Reads the full Message string from StationInfo (not the
             * truncated cell text) and pushes it to the clipboard.  The
             * Copy item is disabled when the message is empty so the
             * menu still surfaces the affordance but doesn't no-op. */
            grid.CellMouseDown += (s, e) =>
            {
                if (e.Button != MouseButtons.Right) return;
                if (e.RowIndex < 0 || e.RowIndex >= grid.Rows.Count) return;
                if (e.ColumnIndex != COL_MSG) return;

                string sid = grid.Rows[e.RowIndex].Tag as string;
                StationInfo st = null;
                if (!string.IsNullOrEmpty(sid))
                    _client.Stations.TryGetValue(sid, out st);
                string msg = st?.Message ?? "";

                var menu = new ContextMenuStrip();
                var item = new ToolStripMenuItem("Copy Message")
                {
                    Enabled = !string.IsNullOrEmpty(msg),
                };
                item.Click += (ms, me) =>
                {
                    try { Clipboard.SetText(msg); } catch { }
                };
                menu.Items.Add(item);

                /* Show at the cursor.  Cell rectangle would also work
                 * but cursor placement matches WinForms convention and
                 * what users expect from a right-click. */
                menu.Show(Cursor.Position);
            };
        }

        private void BuildColumns()
        {
            int[] widths = { 90, 70, 60, 50, 90, 90, 60, 70, 220, 80, 80, 70, 50, 80 };
            for (int i = 0; i < COL_HEADERS.Length; i++)
            {
                bool numeric = (i == COL_KM || i == COL_HDG || i == COL_MHZ || i == COL_SNR);
                var style = new DataGridViewCellStyle
                {
                    Alignment = numeric ? DataGridViewContentAlignment.MiddleRight
                                        : DataGridViewContentAlignment.MiddleLeft,
                };
                /* MHz: render with 4 fixed decimals (pad with trailing
                 * zeros, e.g. 7.177 -> "7.1770", 14.236 -> "14.2360").
                 * Format runs only when CellFormatting hasn't already
                 * substituted the UNKNOWN_NUMBER sentinel with "", so
                 * blank cells stay blank and sorting remains numeric. */
                if (i == COL_MHZ) style.Format = "F4";
                var col = new DataGridViewTextBoxColumn
                {
                    HeaderText = COL_HEADERS[i],
                    Name = "col" + i,
                    Width = widths[i],
                    SortMode = DataGridViewColumnSortMode.Automatic,
                    DefaultCellStyle = style,
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
            _suppressScrollEvent = true;
            try { SyncRowsInner(); }
            finally
            {
                _suppressSelectionEvents = false;
                _suppressScrollEvent = false;
            }
        }

        private void SyncRowsInner()
        {

            /* "Track RX1/RX2" mode: pull the tracked receiver's frequency
             * from the manager.  In TrackMode.Band, snap _bandFilter to
             * the matching ham band so the existing band-filter code
             * paths keep working.  In TrackMode.Frequency the band combo
             * is irrelevant and is left alone -- visibility is decided
             * per-row below by an exact-frequency match. */
            ulong trackFreq = 0UL;
            if      (_trackTarget == TrackTarget.Rx1) trackFreq = FreeDVReporterManager.CurrentFrequencyHz;
            else if (_trackTarget == TrackTarget.Rx2) trackFreq = FreeDVReporterManager.CurrentFrequencyRx2Hz;
            bool tracking = _trackTarget != TrackTarget.Off;
            if (tracking && trackFreq > 0 && _trackMode == TrackMode.Band)
            {
                HamBand b = BandPlan.FromHz(trackFreq);
                if (b != _bandFilter)
                {
                    _bandFilter = b;
                    ApplyBandToCombo();
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

                    bool inFilter;
                    if (tracking && _trackMode == TrackMode.Frequency)
                    {
                        /* Show only stations within +/- TRACK_FREQ_TOL_HZ
                         * of the radio's current VFOA frequency.  When
                         * the radio's freq is unknown (==0) hide all. */
                        if (trackFreq == 0 || st.FrequencyHz == 0)
                            inFilter = false;
                        else
                        {
                            long delta = (long)st.FrequencyHz - (long)trackFreq;
                            if (delta < 0) delta = -delta;
                            inFilter = (delta <= TRACK_FREQ_TOL_HZ);
                        }
                    }
                    else
                    {
                        /* Band filter -- existing behaviour, also used
                         * by TrackMode.Band (which snaps _bandFilter
                         * upstream). */
                        inFilter = _bandFilter == HamBand.All ||
                                   (st.FrequencyHz > 0 && BandPlan.FromHz(st.FrequencyHz) == _bandFilter);
                    }
                    bool tooIdle = _idleMinutes > 0 && st.LastUpdateUtc != DateTime.MinValue &&
                                   (nowUtc - st.LastUpdateUtc).TotalSeconds > idleCutSec;
                    bool visible = inFilter && !tooIdle;

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
                    /* MHz with up to 4 decimals: kHz/1000 then round.
                     * Sorting stays numeric because the cell holds the
                     * rounded double, not a pre-formatted string. */
                    SetCell(row, COL_MHZ, st.FrequencyHz > 0 ? Math.Round(st.FrequencyHz / 1.0e6, 4) : UNKNOWN_NUMBER);
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

                /* Sticky-top scroll.  Keeps the visual top anchored at
                 * the first visible row after every refresh so new
                 * stations sorting near the top don't push existing
                 * top rows off the top edge on a small window.  The
                 * stickiness disengages the moment the user scrolls
                 * away from the top (see grid.Scroll handler in
                 * BuildGrid) and re-engages the moment they scroll
                 * back to it -- so manual browsing is preserved. */
                if (_stickToTop)
                {
                    try
                    {
                        int top = 0;
                        while (top < grid.Rows.Count && !grid.Rows[top].Visible) top++;
                        if (top < grid.Rows.Count && grid.FirstDisplayedScrollingRowIndex != top)
                            grid.FirstDisplayedScrollingRowIndex = top;
                    }
                    catch { }
                }
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
