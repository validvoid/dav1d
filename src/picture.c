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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/intops.h"
#include "common/mem.h"
#include "common/validate.h"

#include "src/picture.h"
#include "src/ref.h"
#include "src/thread.h"

static int picture_alloc_with_edges(Dav1dPicture *const p,
                                    const int w, const int h,
                                    const enum Dav1dPixelLayout layout,
                                    const int bpc,
                                    const int extra, void **const extra_ptr)
{
    int aligned_h;

    if (p->data[0]) {
        fprintf(stderr, "Picture already allocated!\n");
        return -1;
    }
    assert(bpc > 0 && bpc <= 16);

    const int hbd = bpc > 8;
    const int aligned_w = (w + 127) & ~127;
    const int has_chroma = layout != DAV1D_PIXEL_LAYOUT_I400;
    const int ss_ver = layout == DAV1D_PIXEL_LAYOUT_I420;
    const int ss_hor = layout != DAV1D_PIXEL_LAYOUT_I444;
    p->stride[0] = aligned_w << hbd;
    p->stride[1] = has_chroma ? (aligned_w >> ss_hor) << hbd : 0;
    p->p.w = w;
    p->p.h = h;
    p->p.pri = DAV1D_COLOR_PRI_UNKNOWN;
    p->p.trc = DAV1D_TRC_UNKNOWN;
    p->p.mtrx = DAV1D_MC_UNKNOWN;
    p->p.chr = DAV1D_CHR_UNKNOWN;
    aligned_h = (h + 127) & ~127;
    p->p.layout = layout;
    p->p.bpc = bpc;
    const size_t y_sz = p->stride[0] * aligned_h;
    const size_t uv_sz = p->stride[1] * (aligned_h >> ss_ver);
    if (!(p->ref = dav1d_ref_create(y_sz + 2 * uv_sz + extra))) {
        fprintf(stderr, "Failed to allocate memory of size %zu: %s\n",
                y_sz + 2 * uv_sz + extra, strerror(errno));
        return -ENOMEM;
    }
    uint8_t *data = p->ref->data;
    p->data[0] = data;
    p->data[1] = has_chroma ? data + y_sz : NULL;
    p->data[2] = has_chroma ? data + y_sz + uv_sz : NULL;

    if (extra)
        *extra_ptr = &data[y_sz + uv_sz * 2];

    return 0;
}

int dav1d_thread_picture_alloc(Dav1dThreadPicture *const p,
                               const int w, const int h,
                               const enum Dav1dPixelLayout layout, const int bpc,
                               struct thread_data *const t, const int visible)
{
    p->t = t;

    const int res =
        picture_alloc_with_edges(&p->p, w, h, layout, bpc,
                                 t != NULL ? sizeof(atomic_int) * 2 : 0,
                                 (void **) &p->progress);

    p->visible = visible;
    p->flushed = 0;
    if (t) {
        atomic_init(&p->progress[0], 0);
        atomic_init(&p->progress[1], 0);
    }
    return res;
}

void dav1d_picture_ref(Dav1dPicture *const dst, const Dav1dPicture *const src) {
    validate_input(dst != NULL);
    validate_input(dst->data[0] == NULL);
    validate_input(src != NULL);

    if (src->ref) {
        validate_input(src->data[0] != NULL);
        dav1d_ref_inc(src->ref);
    }
    *dst = *src;
}

void dav1d_thread_picture_ref(Dav1dThreadPicture *dst,
                              const Dav1dThreadPicture *src)
{
    dav1d_picture_ref(&dst->p, &src->p);
    dst->t = src->t;
    dst->visible = src->visible;
    dst->progress = src->progress;
    dst->flushed = src->flushed;
}

void dav1d_picture_unref(Dav1dPicture *const p) {
    validate_input(p != NULL);

    if (p->ref) {
        validate_input(p->data[0] != NULL);
        dav1d_ref_dec(p->ref);
    }
    memset(p, 0, sizeof(*p));
}

void dav1d_thread_picture_unref(Dav1dThreadPicture *const p) {
    dav1d_picture_unref(&p->p);

    p->t = NULL;
    p->progress = NULL;
}

void dav1d_thread_picture_wait(const Dav1dThreadPicture *const p,
                               int y_unclipped, const enum PlaneType plane_type)
{
    assert(plane_type != PLANE_TYPE_ALL);

    if (!p->t)
        return;

    // convert to luma units; include plane delay from loopfilters; clip
    const int ss_ver = p->p.p.layout == DAV1D_PIXEL_LAYOUT_I420;
    y_unclipped *= 1 << (plane_type & ss_ver); // we rely here on PLANE_TYPE_UV being 1
    y_unclipped += (plane_type != PLANE_TYPE_BLOCK) * 8; // delay imposed by loopfilter
    const int y = iclip(y_unclipped, 1, p->p.p.h);
    atomic_uint *const progress = &p->progress[plane_type != PLANE_TYPE_BLOCK];

    if (atomic_load_explicit(progress, memory_order_acquire) >= y)
        return;

    pthread_mutex_lock(&p->t->lock);
    while (atomic_load_explicit(progress, memory_order_relaxed) < y)
        pthread_cond_wait(&p->t->cond, &p->t->lock);
    pthread_mutex_unlock(&p->t->lock);
}

void dav1d_thread_picture_signal(const Dav1dThreadPicture *const p,
                                 const int y, // in pixel units
                                 const enum PlaneType plane_type)
{
    assert(plane_type != PLANE_TYPE_UV);

    if (!p->t)
        return;

    pthread_mutex_lock(&p->t->lock);
    if (plane_type != PLANE_TYPE_Y) atomic_store(&p->progress[0], y);
    if (plane_type != PLANE_TYPE_BLOCK) atomic_store(&p->progress[1], y);
    pthread_cond_broadcast(&p->t->cond);
    pthread_mutex_unlock(&p->t->lock);
}
