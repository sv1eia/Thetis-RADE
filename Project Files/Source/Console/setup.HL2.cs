// setup.HL2.cs
//
// MI0BOT: HL2 setup-form event handlers and helpers, factored into a
// partial-class file to keep the main setup.cs untouched.
// Phase 3 port of the Hermes-Lite 2 support from Thetis-hl2.
//
// All members in this file are private/internal to the Setup partial
// class declared in setup.cs.
/*
----------------------------------------------------------------------------------------------
Modified by Christos Nikolaou (SV1EIA) 2026 -- thetis-rade fork.
Christos Nikolaou can be reached by email at : sv1eia@gmail.com
----------------------------------------------------------------------------------------------
*/

using System;
using System.ComponentModel;
using System.Drawing;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Thetis
{
    public partial class Setup : Form
    {
        // MI0BOT: I/O Board LED strip update — called from console.UpdateIOBoard worker (Phase 5)
        public void UpdateIOLedStrip(bool tx, byte bits)
        {
            ucIOPinsLedStripHF.TX = tx;
            ucIOPinsLedStripHF.Bits = (int)bits;
        }

        // MI0BOT: HL2 I/O-Board LED strip — read the output pins over I2C and show them
        private void ucOutPinsLedStripHF_Click(object sender, EventArgs e)
        {
            byte[] read_data = new byte[4];
            int status = 0;
            int timeout = 0;

            if (HL2IOBoardPresent == true)
            {
                console.SetI2CPollingPause(true);

                while (0 != NetworkIO.I2CReadInitiate(1, 0x1d, 169))
                {
                    Thread.Sleep(1);
                    if (timeout++ >= 20) break;
                }

                if (timeout < 20)
                {
                    do
                    {
                        Thread.Sleep(1);
                        status = NetworkIO.I2CResponse(read_data);
                        if (timeout++ >= 20) break;
                    } while (1 == status);

                    if (status == 0)
                        ucOutPinsLedStripHF.Bits = read_data[3];
                }

                console.SetI2CPollingPause(false);
            }
        }

        // MI0BOT: HL2 I/O-Board LED strip — toggle an output pin over I2C
        private void ucOutPinsLedStripHF_MouseDown(object sender, MouseEventArgs e)
        {
            if (HL2IOBoardPresent == true)
            {
                if (chkIOPinControl.Checked)
                {
                    int bit = e.Location.X / 16;
                    byte mask = (byte)(1 << bit);

                    console.SetI2CPollingPause(true);

                    NetworkIO.I2CWrite(1, 0x1d, 169, ucOutPinsLedStripHF.Bits ^ mask);

                    console.SetI2CPollingPause(false);

                    ucOutPinsLedStripHF_Click(sender, e);
                }
            }
        }

        // MI0BOT: HL2 access to I2C bus
        private void chkI2CWriteEnable_CheckedChanged(object sender, EventArgs e)
        {
            if (chkI2CWriteEnable.Checked)
            {
                btnI2CWrite.Enabled = true;
                udI2CWriteData.Enabled = true;
                labelI2CWriteData.Enabled = true;
            }
            else
            {
                btnI2CWrite.Enabled = false;
                udI2CWriteData.Enabled = false;
                labelI2CWriteData.Enabled = false;
            }
        }

        // MI0BOT: HL2 access to I2C bus
        private void btnI2CWrite_MouseDown(object sender, MouseEventArgs e)
        {
            if (!console.PowerOn)
            {
                MessageBox.Show("Power must be on to access the I2C bus.",
                    "Power is off",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Hand);
                return;
            }

            console.SetI2CPollingPause(true);

            int bus = radI2C1.Checked ? 0 : 1;

            int controlReg = (int)((udI2CControl1.Value * 16) + udI2CControl0.Value);

            NetworkIO.I2CWrite(bus, (int)udI2CAddress.Value, controlReg, (int)udI2CWriteData.Value);

            if (controlReg == 169)
            {
                ucOutPinsLedStripHF_Click(sender, e);
            }

            console.SetI2CPollingPause(false);
        }

        // MI0BOT: HL2 access to I2C bus
        private void btnI2CRead_MouseDown(object sender, MouseEventArgs e)
        {
            if (!console.PowerOn)
            {
                MessageBox.Show("Power must be on to access the I2C bus.",
                    "Power is off",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Hand);
                return;
            }

            int bus = radI2C1.Checked ? 0 : 1;
            byte[] read_data = new byte[4];
            int status;
            int timeout = 0;

            console.SetI2CPollingPause(true);

            while (0 != NetworkIO.I2CReadInitiate(bus, (int)udI2CAddress.Value, (int)((udI2CControl1.Value * 16) + udI2CControl0.Value)))
            {
                Thread.Sleep(1);
            }

            do
            {
                Thread.Sleep(1);
                status = NetworkIO.I2CResponse(read_data);
                if (timeout++ >= 20) break;
            } while (1 == status);

            if (-1 == status)
            {
                txtI2CByte0.Text = "or";
                txtI2CByte1.Text = "Err";
                txtI2CByte2.Text = "or";
                txtI2CByte3.Text = "Err";
                txtI2CByte0.ForeColor = Color.Red;
                txtI2CByte1.ForeColor = Color.Red;
                txtI2CByte2.ForeColor = Color.Red;
                txtI2CByte3.ForeColor = Color.Red;
            }
            else
            {
                int byte0, byte1, byte2, byte3;

                byte0 = read_data[3];
                byte1 = read_data[2];
                byte2 = read_data[1];
                byte3 = read_data[0];

                txtI2CByte0.ForeColor = Color.Black;
                txtI2CByte1.ForeColor = Color.Black;
                txtI2CByte2.ForeColor = Color.Black;
                txtI2CByte3.ForeColor = Color.Black;
                txtI2CByte0.Text = byte0.ToString("X2");
                txtI2CByte1.Text = byte1.ToString("X2");
                txtI2CByte2.Text = byte2.ToString("X2");
                txtI2CByte3.Text = byte3.ToString("X2");
            }

            console.SetI2CPollingPause(false);
        }

        // MI0BOT: Hide HL2-only ancillary controls AND restore every relabelled
        //         mainline control back to its Setup.Designer.cs default text /
        //         tooltip / visibility / value, so that switching FROM HL2 to
        //         any other radio model leaves the form in the same state
        //         mainline Thetis-RADE shows for that model.  For controls
        //         already re-set per-arm by mainline (labelRXAntControl,
        //         chkRxOutOnTx, tpOtherHW, tpPennyCtrl, tpAlexControl,
        //         chkEXT*OutOnTx, chkAutoATTRx{1,2}.Enabled) no reset is needed
        //         here because the model-switch arm overwrites them.  Anything
        //         the HL2 arm touches that mainline arms do NOT re-set must be
        //         reverted here -- otherwise HL2 values leak into other models
        //         on a model switch.
        private void removeHL2Options()
        {
            // chkCATtoVFOB lives in grpCatControlBox -- always shown.  Hide it
            // for non-HL2 models so the HL2-only VFO-B redirect is invisible.
            chkCATtoVFOB.Enabled = false;
            chkCATtoVFOB.Visible = false;

            // chkApolloPresent / chkHL2IOBoardPresent share Point(4, 23) inside
            // pnlAlexApollo.  HL2 arm hides Apollo and shows HL2 I/O Board;
            // restore the Designer defaults here so non-HL2 models see Apollo
            // again (the model arm that follows will overwrite .Enabled /
            // .Checked appropriately).
            chkApolloPresent.Visible = true;
            chkHL2IOBoardPresent.Visible = false;
            chkHL2IOBoardPresent.Enabled = false;
            chkHL2IOBoardPresent.Checked = false;

            // Value-typed controls the HL2 arm widens.  Other model arms do
            // not touch these, so restore Designer defaults explicitly.
            //   udATTOnTX:                  Minimum 0  (HL2 sets -28)
            //   udHermesStepAttenuatorDataRX2: Minimum 0 (HL2 sets -28)
            //   udTXTunePower: percentage in [0..100], integer step
            //                  (HL2 sets dB scale [-16.5..0], 0.5 step, 1 dp)
            try
            {
                udATTOnTX.Minimum = (decimal)0;
                udHermesStepAttenuatorDataRX2.Minimum = (decimal)0;
                udTXTunePower.Minimum = (decimal)0;
                udTXTunePower.Maximum = (decimal)100;
                udTXTunePower.Increment = (decimal)1;
                udTXTunePower.DecimalPlaces = 0;
            }
            catch { /* clamp guards a transient inversion when Min/Max overlap */ }

            // Designer-default text / tooltip / visibility (mainline never
            // re-sets these):
            chkApolloFilter.Text = "Enable Filters";
            chkApolloTuner.Text = "Enable Tuner";
            grpApolloCtrl.Text = "Apollo Control";
            tpApolloApollo.Text = "Apollo";
            chkHERCULES.Text = "Hercules Amp";
            chkHERCULES.Visible = false;
            grpDisplay8000DLE.Text = "7000/8000/G2/G2E/ANV/RP";
            chkANAN8000DLEDisplayVoltsAmps.Text = "Show Volts/Amps";

            toolTip1.SetToolTip(chkApolloFilter, "Enables the LPF on Apollo");
            toolTip1.SetToolTip(chkApolloTuner, "Enables Apollo ATU");
            toolTip1.SetToolTip(chkHERCULES, "Preset pins for Hercules Amplifier");

            // MI0BOT: revert HL2-only UI states the model arm sets that no other
            //         model arm re-sets, so switching away from HL2 is clean.
            chkRX2StepAtt.Visible = true;
            udHermesStepAttenuatorDataRX2.Visible = true;
            chkDisableRXOut.Enabled = true;
            chkMercDither.Enabled = true;
            chkMercRandom.Enabled = true;
            tpPennyCtrl.Text = "Penny/Hermes Ctrl";
            ucIOPinsLedStripHF.DisplayBits = 7;
        }

        // MI0BOT: Backing field for HL2IOBoardPresent
        private bool hl2IOBoardPresent = false;

        // MI0BOT: HL2 I/O Board auto-detected flag — set by the I/O Board worker thread (Phase 5).
        public bool HL2IOBoardPresent
        {
            get { return hl2IOBoardPresent; }
            set
            {
                hl2IOBoardPresent = value;
                chkHL2IOBoardPresent.Checked = value;
                chkHL2IOBoardPresent.Enabled = value;
                ucIOPinsLedStripHF.Enabled = value;
                grpIOPinState.Enabled = value;
            }
        }

        // MI0BOT: Control band volts for the HL2 (ADC dither bit)
        private void chkHL2BandVolts_CheckedChanged(object sender, System.EventArgs e)
        {
            if (initializing) return;
            int v = chkHL2BandVolts.Checked ? 1 : 0;
            NetworkIO.SetADCDither(v);
        }

        // MI0BOT: Control power supply sync for the HL2 (ADC random bit)
        private void chkHL2PsSync_CheckedChanged(object sender, System.EventArgs e)
        {
            if (initializing) return;
            int v = chkHL2PsSync.Checked ? 1 : 0;
            NetworkIO.SetADCRandom(v);
        }

        // MI0BOT: Controls the hardware tx buffer in the HL2
        private void udTxBufferLat_ValueChanged(object sender, EventArgs e)
        {
            NetworkIO.SetTxLatency((int)udTxBufferLat.Value);
        }

        // MI0BOT: Controls the hardware PTT hang in the HL2
        private void udPTTHang_ValueChanged(object sender, EventArgs e)
        {
            NetworkIO.SetPttHang((int)udPTTHang.Value);
        }

        // MI0BOT: Controls the ability to send CAT commands to VFO B (HL2 single-VFO workaround)
        private void chkCATtoVFOB_CheckedChanged(object sender, EventArgs e)
        {
            console.CATtoVFOB = chkCATtoVFOB.Checked;
        }

        // MI0BOT: Controls if the HL2 will reset after an Ethernet disconnect
        private void chkDisconnectReset_CheckedChanged(object sender, EventArgs e)
        {
            int v = chkDisconnectReset.Checked ? 1 : 0;
            NetworkIO.SetResetOnDisconnect(v);
        }

        // MI0BOT: HL2 I/O Board auto-detect indicator (read-only checkbox)
        private void chkHL2IOBoardPresent_CheckedChanged(object sender, EventArgs e)
        {
            HL2IOBoardPresent = chkHL2IOBoardPresent.Checked;
        }

        // MI0BOT: Swap L/R audio channels sent over P1 (HL2 firmware fudge)
        private void chkSwapAudioChannels_CheckedChanged(object sender, EventArgs e)
        {
            int swap = chkSwapAudioChannels.Checked ? 1 : 0;
            NetworkIO.SwapAudioChannels(swap);
        }

        // MI0BOT: HL2 access to I2C bus (master enable for the I2C panel)
        private void chkI2CEnable_CheckedChanged(object sender, EventArgs e)
        {
            groupBoxI2CControl.Enabled = chkI2CEnable.Checked;
        }

        // MI0BOT: Support for HL2 10MHz input
        public bool Ext10MHzChecked
        {
            get { return chkExt10MHz.Checked; }
            set { }
        }

        // MI0BOT: Support for HL2 Cl2 clock output
        public bool Cl2Checked
        {
            get { return chkCl2Enable.Checked; }
            set { }
        }

        // MI0BOT: Data to program clock generator in HL2 to accept external 10MHz on CL2
        //         Data in format of Address, Data
        private byte[] clockRegisterData10MhzEnable = new byte[] {
            0x10, 0xc0,
            0x13, 0x03,
            0x10, 0x40,
            0x2d, 0x01,
            0x2e, 0x20,
            0x22, 0x03,
            0x23, 0x00,
            0x24, 0x00,
            0x25, 0x00,
            0x19, 0x00,
            0x1A, 0x00,
            0x1B, 0x00,
            0x18, 0x00,
            0x17, 0x12 };

        private byte[] clockRegisterData10MhzDisable = new byte[] {
            0x10, 0xc0,
            0x13, 0x00,
            0x10, 0x80,
            0x2d, 0x01,
            0x2e, 0x10,
            0x22, 0x00,
            0x23, 0x00,
            0x24, 0x00,
            0x25, 0x00,
            0x19, 0x00,
            0x1A, 0x00,
            0x1B, 0x00,
            0x18, 0x40,
            0x17, 0x04 };

        private byte[] clockRegisterDataCl2 = new byte[] {
            0x62, 0x3b,
            0x2c, 0x00,
            0x31, 0x81,
            0x3d, 0x01,
            0x3e, 0x10,
            0x32, 0x00,
            0x33, 0x00,
            0x34, 0x00,
            0x35, 0x00,
            0x63, 0x01 };

        private byte[] clockRegisterDataCl2Off = new byte[] {
            0x62, 0x5b,
            0x2c, 0x00,
            0x31, 0x00,
            0x3d, 0x00,
            0x3e, 0x00,
            0x32, 0x00,
            0x33, 0x00,
            0x34, 0x00,
            0x35, 0x00,
            0x63, 0x00 };

        // MI0BOT: Support for HL2 Cl2 clock output
        private async Task WriteVersaClockAsync(byte[] registerData)
        {
            byte Timeout = 0;
            int status = 0;

            if (!initializing)
            {
                if (!console.PowerOn)
                {
                    MessageBox.Show("Power must be on to set the CL2 clock frequency.",
                        "Power is off",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Hand);
                    return;
                }

                console.SetI2CPollingPause(true);

                for (int i = 0; i < registerData.Length; i += 2)
                {
                    do
                    {
                        status = NetworkIO.I2CWrite(0, 0xd4, (int)registerData[i], registerData[i + 1]);
                        if (Timeout++ >= 50) break;
                        await Task.Delay(1);
                    } while (status != 0);

                    if (50 <= Timeout)
                    {
                        MessageBox.Show("IC2 timed out.",
                            "IC2 Fail",
                            MessageBoxButtons.OK,
                            MessageBoxIcon.Hand);
                        break;
                    }

                    if (status == -1)
                    {
                        MessageBox.Show("IC2 write failed.",
                            "IC2 Fail",
                            MessageBoxButtons.OK,
                            MessageBoxIcon.Hand);
                        break;
                    }
                }

                console.SetI2CPollingPause(false);
            }
        }

        // MI0BOT: Support for HL2 10MHz clock input
        public void EnableCl1_10MHz()
        {
            WriteVersaClockAsync(clockRegisterData10MhzEnable);
        }

        // MI0BOT: Support for HL2 10MHz clock input
        public void DisableCl1_10MHz()
        {
            WriteVersaClockAsync(clockRegisterData10MhzDisable);
        }

        // MI0BOT: Support for HL2 Cl2 clock output
        public void ControlCl2(bool enable)
        {
            Decimal vco = (Decimal)1305.6;

            if (chkExt10MHz.Checked)
                vco = 1440;

            if (enable)
            {
                udCl2Freq.Enabled = true;

                if (0 != udCl2Freq.Value)
                {
                    Decimal diviser = vco / udCl2Freq.Value;
                    int integer = (int)Decimal.Truncate(diviser);
                    clockRegisterDataCl2[7] = (Byte)((integer >> 4) & 0xff);
                    clockRegisterDataCl2[9] = (Byte)((integer << 4) & 0xf0);

                    Decimal frac = diviser - integer;
                    int intFrac = (int)(frac * (Decimal)(1 << 24));

                    clockRegisterDataCl2[11] = (Byte)((intFrac >> 22) & 0xff);
                    clockRegisterDataCl2[13] = (Byte)((intFrac >> 14) & 0xff);
                    clockRegisterDataCl2[15] = (Byte)((intFrac >> 6) & 0xff);
                    clockRegisterDataCl2[17] = (Byte)((intFrac << 2) & 0xf6);

                    WriteVersaClockAsync(clockRegisterDataCl2);
                }
            }
            else
            {
                udCl2Freq.Enabled = false;
                WriteVersaClockAsync(clockRegisterDataCl2Off);
            }
        }

        // MI0BOT: Support for HL2 Cl2 clock output
        private void chkCl2Enable_CheckedChanged(object sender, EventArgs e)
        {
            ControlCl2(chkCl2Enable.Checked);
        }

        // MI0BOT: Support for HL2 Cl2 clock output
        private void udCl2Freq_ValueChanged(object sender, EventArgs e)
        {
            ControlCl2(chkCl2Enable.Checked);
        }

        // MI0BOT: Support for HL2 10MHz clock input
        private void chkExt10MHz_CheckedChanged(object sender, EventArgs e)
        {
            if (chkExt10MHz.Checked)
                EnableCl1_10MHz();
            else
                DisableCl1_10MHz();

            ControlCl2(chkCl2Enable.Checked);
        }
    }
}
