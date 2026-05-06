/*  Maidenhead.cs
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

namespace Thetis.FreeDVReporter
{
    public static class Maidenhead
    {
        public const double EarthRadiusKm = 6371.0;

        /* Try to parse a 4 or 6 character Maidenhead locator into the
         * centre lat/lon of the square.  Returns false on garbage input.
         * Output longitudes are in -180..180, latitudes in -90..90. */
        public static bool TryToLatLon(string grid, out double lat, out double lon)
        {
            lat = 0; lon = 0;
            if (string.IsNullOrEmpty(grid)) return false;
            grid = grid.Trim().ToUpperInvariant();
            if (grid.Length != 4 && grid.Length != 6) return false;
            if (grid[0] < 'A' || grid[0] > 'R') return false;
            if (grid[1] < 'A' || grid[1] > 'R') return false;
            if (grid[2] < '0' || grid[2] > '9') return false;
            if (grid[3] < '0' || grid[3] > '9') return false;

            double lonField   = (grid[0] - 'A') * 20.0 - 180.0;
            double latField   = (grid[1] - 'A') * 10.0 - 90.0;
            double lonSquare  = (grid[2] - '0') * 2.0;
            double latSquare  = (grid[3] - '0') * 1.0;
            double lonSubsq, latSubsq;

            if (grid.Length == 6)
            {
                if (grid[4] < 'A' || grid[4] > 'X') return false;
                if (grid[5] < 'A' || grid[5] > 'X') return false;
                lonSubsq = (grid[4] - 'A') * (2.0 / 24.0);
                latSubsq = (grid[5] - 'A') * (1.0 / 24.0);
                /* +half-subsquare to land in centre */
                lon = lonField + lonSquare + lonSubsq + (1.0 / 24.0);
                lat = latField + latSquare + latSubsq + (0.5 / 24.0);
            }
            else
            {
                /* 4-char: centre of the 2 deg lon x 1 deg lat square */
                lon = lonField + lonSquare + 1.0;
                lat = latField + latSquare + 0.5;
            }
            return true;
        }

        /* Great-circle distance between two grids in kilometres, or NaN
         * if either grid is invalid. */
        public static double DistanceKm(string gridA, string gridB)
        {
            double aLat, aLon, bLat, bLon;
            if (!TryToLatLon(gridA, out aLat, out aLon)) return double.NaN;
            if (!TryToLatLon(gridB, out bLat, out bLon)) return double.NaN;
            return DistanceKmLatLon(aLat, aLon, bLat, bLon);
        }

        public static double DistanceKmLatLon(double aLat, double aLon, double bLat, double bLon)
        {
            double phi1 = Deg2Rad(aLat), phi2 = Deg2Rad(bLat);
            double dphi = Deg2Rad(bLat - aLat), dlam = Deg2Rad(bLon - aLon);
            double s = Math.Sin(dphi / 2);
            double t = Math.Sin(dlam / 2);
            double a = s * s + Math.Cos(phi1) * Math.Cos(phi2) * t * t;
            double c = 2 * Math.Atan2(Math.Sqrt(a), Math.Sqrt(1 - a));
            return EarthRadiusKm * c;
        }

        /* Initial great-circle bearing from gridA to gridB in degrees
         * (0..360, where 0 = north).  NaN on invalid input. */
        public static double BearingDeg(string gridA, string gridB)
        {
            double aLat, aLon, bLat, bLon;
            if (!TryToLatLon(gridA, out aLat, out aLon)) return double.NaN;
            if (!TryToLatLon(gridB, out bLat, out bLon)) return double.NaN;
            double phi1 = Deg2Rad(aLat), phi2 = Deg2Rad(bLat);
            double dlam = Deg2Rad(bLon - aLon);
            double y = Math.Sin(dlam) * Math.Cos(phi2);
            double x = Math.Cos(phi1) * Math.Sin(phi2)
                     - Math.Sin(phi1) * Math.Cos(phi2) * Math.Cos(dlam);
            double brng = Rad2Deg(Math.Atan2(y, x));
            if (brng < 0) brng += 360.0;
            return brng;
        }

        private static double Deg2Rad(double d) { return d * Math.PI / 180.0; }
        private static double Rad2Deg(double r) { return r * 180.0 / Math.PI; }
    }

    /* ============================================================
     * Amateur HF/V/UHF band-edge mapping that mirrors FreeDV-GUI's
     * filter_frequency table.  Frequency in Hz.
     * ============================================================ */
    public enum HamBand
    {
        All, B160m, B80m, B60m, B40m, B30m, B20m, B17m, B15m, B12m, B10m, B6m, VhfUhf, Other
    }

    public static class BandPlan
    {
        public static HamBand FromHz(ulong hz)
        {
            double mhz = hz / 1e6;
            if (mhz >= 1.8   && mhz <= 2.0)     return HamBand.B160m;
            if (mhz >= 3.5   && mhz <= 4.0)     return HamBand.B80m;
            if (mhz >= 5.25  && mhz <= 5.45)    return HamBand.B60m;
            if (mhz >= 7.0   && mhz <= 7.3)     return HamBand.B40m;
            if (mhz >= 10.1  && mhz <= 10.15)   return HamBand.B30m;
            if (mhz >= 14.0  && mhz <= 14.35)   return HamBand.B20m;
            if (mhz >= 18.068&& mhz <= 18.168)  return HamBand.B17m;
            if (mhz >= 21.0  && mhz <= 21.45)   return HamBand.B15m;
            if (mhz >= 24.89 && mhz <= 24.99)   return HamBand.B12m;
            if (mhz >= 28.0  && mhz <= 29.7)    return HamBand.B10m;
            if (mhz >= 50.0  && mhz <= 54.0)    return HamBand.B6m;
            if (mhz >= 144.0)                   return HamBand.VhfUhf;
            return HamBand.Other;
        }

        public static string Label(HamBand b)
        {
            switch (b)
            {
                case HamBand.All:    return "All";
                case HamBand.B160m:  return "160m";
                case HamBand.B80m:   return "80m";
                case HamBand.B60m:   return "60m";
                case HamBand.B40m:   return "40m";
                case HamBand.B30m:   return "30m";
                case HamBand.B20m:   return "20m";
                case HamBand.B17m:   return "17m";
                case HamBand.B15m:   return "15m";
                case HamBand.B12m:   return "12m";
                case HamBand.B10m:   return "10m";
                case HamBand.B6m:    return "6m";
                case HamBand.VhfUhf: return "VHF+UHF";
                case HamBand.Other:  return "Other";
            }
            return "?";
        }
    }
}
