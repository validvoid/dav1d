/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <assert.h>

#include "common/intops.h"

#include "src/getbits.h"

void init_get_bits(GetBits *const c,
                   const uint8_t *const data, const size_t sz)
{
    c->ptr = c->ptr_start = data;
    c->ptr_end = &c->ptr_start[sz];
    c->bits_left = 0;
    c->state = 0;
    c->error = 0;
    c->eof = 0;
}

static void refill(GetBits *const c, const unsigned n) {
    assert(c->bits_left <= 56);
    uint64_t state = 0;
    do {
        state <<= 8;
        c->bits_left += 8;
        if (!c->eof)
            state |= *c->ptr++;
        if (c->ptr >= c->ptr_end) {
            c->error = c->eof;
            c->eof = 1;
        }
    } while (n > c->bits_left);
    c->state |= state << (64 - c->bits_left);
}

unsigned get_bits(GetBits *const c, const unsigned n) {
    assert(n <= 32 /* can go up to 57 if we change return type */);

    if (n > c->bits_left) refill(c, n);

    const uint64_t state = c->state;
    c->bits_left -= n;
    c->state <<= n;

    return state >> (64 - n);
}

int get_sbits(GetBits *const c, const unsigned n) {
    const int shift = 31 - n;
    const int res = get_bits(c, n + 1) << shift;
    return res >> shift;
}

unsigned get_uniform(GetBits *const c, const unsigned n) {
    assert(n > 0);
    const int l = ulog2(n) + 1;
    assert(l > 0);
    const int m = (1 << l) - n;
    const int v = get_bits(c, l - 1);
    return v < m ? v : (v << 1) - m + get_bits(c, 1);
}

unsigned get_vlc(GetBits *const c) {
    int n_bits = 0;
    while (!get_bits(c, 1)) n_bits++;
    if (n_bits >= 32) return 0xFFFFFFFFU;
    return ((1 << n_bits) - 1) + get_bits(c, n_bits);
}

static unsigned get_bits_subexp_u(GetBits *const c, const unsigned ref,
                                  const unsigned n)
{
    unsigned v = 0;

    for (int i = 0;; i++) {
        const int b = i ? 3 + i - 1 : 3;

        if (n < v + 3 * (1 << b)) {
            v += get_uniform(c, n - v + 1);
            break;
        }

        if (!get_bits(c, 1)) {
            v += get_bits(c, b);
            break;
        }

        v += 1 << b;
    }

    return ref * 2 <= n ? inv_recenter(ref, v) : n - inv_recenter(n - ref, v);
}

int get_bits_subexp(GetBits *const c, const int ref, const unsigned n) {
    return (int) get_bits_subexp_u(c, ref + (1 << n), 2 << n) - (1 << n);
}

const uint8_t *flush_get_bits(GetBits *c) {
    c->bits_left = 0;
    c->state = 0;
    return c->ptr;
}
