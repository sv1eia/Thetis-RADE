/*  r8brain_wrap.h
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
#ifndef _r8brain_wrap_h
#define _r8brain_wrap_h

#ifdef __cplusplus
extern "C" {
#endif

typedef void* r8b_handle;

/* Construct.  src_rate / dst_rate may be any positive doubles.
 * max_in_len is the maximum number of input samples passed to a single
 * r8b_process_ff() call; r8brain pre-allocates internal scratch sized to
 * this.  Returns NULL on failure. */
r8b_handle r8b_create(double src_rate, double dst_rate, int max_in_len);

/* Tear down.  Safe on NULL. */
void r8b_destroy(r8b_handle h);

/* Process up to n_in samples.  Writes up to out_cap output samples to out;
 * returns the number actually written.  May return zero during the
 * resampler's startup transient (filter group delay) -- the caller should
 * be prepared to receive variable counts and buffer accordingly. */
int r8b_process_ff(r8b_handle h, const float* in, int n_in,
                   float* out, int out_cap);

/* Clear the filter delay-line state and re-prewarm so the next
 * process() call returns output from the first input sample.  Call at
 * MOX RX->TX edges to prevent the first preamble from being convolved
 * with the previous over's tail.  Safe on NULL. */
void r8b_clear(r8b_handle h);

#ifdef __cplusplus
}
#endif

#endif /* _r8brain_wrap_h */
