/*  r8brain_wrap.cpp
 *
 *  C-callable wrapper around r8brain-free-src's CDSPResampler24.
 *
 *  Copyright (C) 2026  Christos Nikolaou (SV1EIA) <sv1eia@gmail.com>
 *  r8brain-free-src upstream: MIT, Aleksey Vaneev.
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

#include <new>
#include <cstring>
#include <cstdlib>

#include "CDSPResampler.h"

#include "r8brain_wrap.h"

/* r8brain expects exactly one translation unit to define this knob.
 * -1 means "auto-select" the number of fractional filter positions. */
namespace r8b { int InterpFilterFracs = -1; }

namespace
{
    struct r8b_state
    {
        r8b::CDSPResampler24* resamp;
        double*               in_d;       /* float -> double scratch */
        int                   max_in_len;
    };

    /* Feed zero samples to the resampler until it is ready to emit
     * output on its first real sample.  r8brain absorbs the first
     * getInLenBeforeOutPos(0) input samples into its polyphase filter
     * delay-line before producing any output; without this pre-warm
     * the first call to r8b_process_ff() with real audio returns 0
     * samples even though it has consumed the input.  Mirrors
     * freedv-gui's ResampleStep::prewarm_(). */
    void r8b_prewarm(r8b_state* s)
    {
        if (!s || !s->resamp || s->max_in_len <= 0 || !s->in_d) return;
        for (int i = 0; i < s->max_in_len; ++i) s->in_d[i] = 0.0;
        double* tmp_out = 0;
        int need = s->resamp->getInLenBeforeOutPos(0);
        while (need > 0)
        {
            int n = (need < s->max_in_len) ? need : s->max_in_len;
            s->resamp->process(s->in_d, n, tmp_out);
            need -= n;
        }
    }
}

extern "C"
r8b_handle r8b_create(double src_rate, double dst_rate, int max_in_len)
{
    if (src_rate <= 0.0 || dst_rate <= 0.0 || max_in_len <= 0)
        return 0;

    r8b_state* s = new (std::nothrow) r8b_state;
    if (!s) return 0;

    s->in_d       = 0;
    s->resamp     = 0;
    s->max_in_len = max_in_len;

    s->in_d = (double*)_aligned_malloc(sizeof(double) * (size_t)max_in_len, 16);
    if (!s->in_d)
    {
        delete s;
        return 0;
    }

    try
    {
        s->resamp = new r8b::CDSPResampler24(src_rate, dst_rate, max_in_len);
    }
    catch (...)
    {
        _aligned_free(s->in_d);
        delete s;
        return 0;
    }

    r8b_prewarm(s);

    return (r8b_handle)s;
}

extern "C"
void r8b_destroy(r8b_handle h)
{
    if (!h) return;
    r8b_state* s = (r8b_state*)h;
    if (s->resamp) delete s->resamp;
    if (s->in_d)   _aligned_free(s->in_d);
    delete s;
}

/* Clear the resampler's filter delay-line state, then pre-warm again
 * so the next process() call produces output from the first sample.
 * Called from the MOX RX->TX edge in radae.c so the first preamble of
 * every over is computed from a clean filter state instead of being
 * convolved with the tail of the previous over. */
extern "C"
void r8b_clear(r8b_handle h)
{
    if (!h) return;
    r8b_state* s = (r8b_state*)h;
    if (!s->resamp) return;
    s->resamp->clear();
    r8b_prewarm(s);
}

extern "C"
int r8b_process_ff(r8b_handle h, const float* in, int n_in,
                   float* out, int out_cap)
{
    if (!h || n_in <= 0 || !out || out_cap <= 0) return 0;

    r8b_state* s = (r8b_state*)h;
    if (n_in > s->max_in_len) n_in = s->max_in_len;

    for (int i = 0; i < n_in; ++i)
        s->in_d[i] = (double)in[i];

    double* op = 0;
    int n_out = s->resamp->process(s->in_d, n_in, op);

    if (n_out <= 0) return 0;
    if (n_out > out_cap) n_out = out_cap;

    for (int i = 0; i < n_out; ++i)
    {
        double v = op[i];
        if (v >  1.0) v =  1.0;
        if (v < -1.0) v = -1.0;
        out[i] = (float)v;
    }

    return n_out;
}
