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

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "dav1d/data.h"

#include "common/intops.h"
#include "common/mem.h"

#include "src/decode.h"
#include "src/dequant_tables.h"
#include "src/env.h"
#include "src/qm.h"
#include "src/recon.h"
#include "src/ref.h"
#include "src/tables.h"
#include "src/thread_task.h"
#include "src/warpmv.h"

static void init_quant_tables(const Av1SequenceHeader *const seq_hdr,
                              const Av1FrameHeader *const frame_hdr,
                              const int qidx, uint16_t (*dq)[3][2])
{
    for (int i = 0; i < (frame_hdr->segmentation.enabled ? 8 : 1); i++) {
        const int yac = frame_hdr->segmentation.enabled ?
            iclip_u8(qidx + frame_hdr->segmentation.seg_data.d[i].delta_q) : qidx;
        const int ydc = iclip_u8(yac + frame_hdr->quant.ydc_delta);
        const int uac = iclip_u8(yac + frame_hdr->quant.uac_delta);
        const int udc = iclip_u8(yac + frame_hdr->quant.udc_delta);
        const int vac = iclip_u8(yac + frame_hdr->quant.vac_delta);
        const int vdc = iclip_u8(yac + frame_hdr->quant.vdc_delta);

        dq[i][0][0] = dav1d_dq_tbl[seq_hdr->bpc > 8][ydc][0];
        dq[i][0][1] = dav1d_dq_tbl[seq_hdr->bpc > 8][yac][1];
        dq[i][1][0] = dav1d_dq_tbl[seq_hdr->bpc > 8][udc][0];
        dq[i][1][1] = dav1d_dq_tbl[seq_hdr->bpc > 8][uac][1];
        dq[i][2][0] = dav1d_dq_tbl[seq_hdr->bpc > 8][vdc][0];
        dq[i][2][1] = dav1d_dq_tbl[seq_hdr->bpc > 8][vac][1];
    }
}

static int read_mv_component_diff(Dav1dTileContext *const t,
                                  CdfMvComponent *const mv_comp,
                                  const int have_fp)
{
    Dav1dTileState *const ts = t->ts;
    const Dav1dFrameContext *const f = t->f;
    const int have_hp = f->frame_hdr.hp;
    const int sign = msac_decode_bool_adapt(&ts->msac, mv_comp->sign);
    const int cl = msac_decode_symbol_adapt(&ts->msac, mv_comp->classes, 11);
    int up, fp, hp;

    if (!cl) {
        up = msac_decode_bool_adapt(&ts->msac, mv_comp->class0);
        if (have_fp) {
            fp = msac_decode_symbol_adapt(&ts->msac, mv_comp->class0_fp[up], 4);
            hp = have_hp ? msac_decode_bool_adapt(&ts->msac, mv_comp->class0_hp) : 1;
        } else {
            fp = 3;
            hp = 1;
        }
    } else {
        up = 1 << cl;
        for (int n = 0; n < cl; n++)
            up |= msac_decode_bool_adapt(&ts->msac, mv_comp->classN[n]) << n;
        if (have_fp) {
            fp = msac_decode_symbol_adapt(&ts->msac, mv_comp->classN_fp, 4);
            hp = have_hp ? msac_decode_bool_adapt(&ts->msac, mv_comp->classN_hp) : 1;
        } else {
            fp = 3;
            hp = 1;
        }
    }

    const int diff = ((up << 3) | (fp << 1) | hp) + 1;

    return sign ? -diff : diff;
}

static void read_mv_residual(Dav1dTileContext *const t, mv *const ref_mv,
                             CdfMvContext *const mv_cdf, const int have_fp)
{
    switch (msac_decode_symbol_adapt(&t->ts->msac, t->ts->cdf.mv.joint, N_MV_JOINTS)) {
    case MV_JOINT_HV:
        ref_mv->y += read_mv_component_diff(t, &mv_cdf->comp[0], have_fp);
        ref_mv->x += read_mv_component_diff(t, &mv_cdf->comp[1], have_fp);
        break;
    case MV_JOINT_H:
        ref_mv->x += read_mv_component_diff(t, &mv_cdf->comp[1], have_fp);
        break;
    case MV_JOINT_V:
        ref_mv->y += read_mv_component_diff(t, &mv_cdf->comp[0], have_fp);
        break;
    default:
        break;
    }
}

static void read_tx_tree(Dav1dTileContext *const t,
                         const enum RectTxfmSize from,
                         const int depth, uint16_t *const masks,
                         const int x_off, const int y_off)
{
    const Dav1dFrameContext *const f = t->f;
    const int bx4 = t->bx & 31, by4 = t->by & 31;
    const TxfmInfo *const t_dim = &av1_txfm_dimensions[from];
    const int txw = t_dim->lw, txh = t_dim->lh;
    int is_split;

    if (depth < 2 && from > (int) TX_4X4) {
        const int cat = 2 * (TX_64X64 - t_dim->max) - depth;
        const int a = t->a->tx[bx4] < txw;
        const int l = t->l.tx[by4] < txh;

        is_split = msac_decode_bool_adapt(&t->ts->msac, t->ts->cdf.m.txpart[cat][a + l]);
        if (is_split)
            masks[depth] |= 1 << (y_off * 4 + x_off);
    } else {
        is_split = 0;
    }

    if (is_split && t_dim->max > TX_8X8) {
        const enum RectTxfmSize sub = t_dim->sub;
        const TxfmInfo *const sub_t_dim = &av1_txfm_dimensions[sub];
        const int txsw = sub_t_dim->w, txsh = sub_t_dim->h;

        read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 0, y_off * 2 + 0);
        t->bx += txsw;
        if (txw >= txh && t->bx < f->bw)
            read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 1, y_off * 2 + 0);
        t->bx -= txsw;
        t->by += txsh;
        if (txh >= txw && t->by < f->bh) {
            read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 0, y_off * 2 + 1);
            t->bx += txsw;
            if (txw >= txh && t->bx < f->bw)
                read_tx_tree(t, sub, depth + 1, masks,
                             x_off * 2 + 1, y_off * 2 + 1);
            t->bx -= txsw;
        }
        t->by -= txsh;
    } else {
        memset(&t->a->tx[bx4], is_split ? TX_4X4 : txw, t_dim->w);
        memset(&t->l.tx[by4], is_split ? TX_4X4 : txh, t_dim->h);
    }
}

int av1_neg_deinterleave(int diff, int ref, int max) {
    if (!ref) return diff;
    if (ref >= (max - 1)) return max - diff - 1;
    if (2 * ref < max) {
        if (diff <= 2 * ref) {
            if (diff & 1)
                return ref + ((diff + 1) >> 1);
            else
                return ref - (diff >> 1);
        }
        return diff;
    } else {
        if (diff <= 2 * (max - ref - 1)) {
            if (diff & 1)
                return ref + ((diff + 1) >> 1);
            else
                return ref - (diff >> 1);
        }
        return max - (diff + 1);
    }
}

static void find_matching_ref(const Dav1dTileContext *const t,
                              const enum EdgeFlags intra_edge_flags,
                              const int bw4, const int bh4,
                              const int w4, const int h4,
                              const int have_left, const int have_top,
                              const int ref, uint64_t masks[2])
{
    const Dav1dFrameContext *const f = t->f;
    const ptrdiff_t b4_stride = f->b4_stride;
    const refmvs *const r = &f->mvs[t->by * b4_stride + t->bx];
    int count = 0;
    int have_topleft = have_top && have_left;
    int have_topright = imax(bw4, bh4) < 32 &&
                        have_top && t->bx + bw4 < t->ts->tiling.col_end &&
                        (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT);

#define bs(rp) av1_block_dimensions[sbtype_to_bs[(rp)->sb_type]]
#define matches(rp) ((rp)->ref[0] == ref + 1 && (rp)->ref[1] == -1)

    if (have_top) {
        const refmvs *r2 = &r[-b4_stride];
        if (matches(r2)) {
            masks[0] |= 1;
            count = 1;
        }
        int aw4 = bs(r2)[0];
        if (aw4 >= bw4) {
            const int off = t->bx & (aw4 - 1);
            if (off) have_topleft = 0;
            if (aw4 - off > bw4) have_topright = 0;
        } else {
            unsigned mask = 1 << aw4;
            for (int x = aw4; x < w4; x += aw4) {
                r2 += aw4;
                if (matches(r2)) {
                    masks[0] |= mask;
                    if (++count >= 8) return;
                }
                aw4 = bs(r2)[0];
                mask <<= aw4;
            }
        }
    }
    if (have_left) {
        const refmvs *r2 = &r[-1];
        if (matches(r2)) {
            masks[1] |= 1;
            if (++count >= 8) return;
        }
        int lh4 = bs(r2)[1];
        if (lh4 >= bh4) {
            if (t->by & (lh4 - 1)) have_topleft = 0;
        } else {
            unsigned mask = 1 << lh4;
            for (int y = lh4; y < h4; y += lh4) {
                r2 += lh4 * b4_stride;
                if (matches(r2)) {
                    masks[1] |= mask;
                    if (++count >= 8) return;
                }
                lh4 = bs(r2)[1];
                mask <<= lh4;
            }
        }
    }
    if (have_topleft && matches(&r[-(1 + b4_stride)])) {
        masks[1] |= 1ULL << 32;
        if (++count >= 8) return;
    }
    if (have_topright && matches(&r[bw4 - b4_stride])) {
        masks[0] |= 1ULL << 32;
    }
#undef matches
}

static void derive_warpmv(const Dav1dTileContext *const t,
                          const int bw4, const int bh4,
                          const uint64_t masks[2], const struct mv mv,
                          WarpedMotionParams *const wmp)
{
    int pts[8][2 /* in, out */][2 /* x, y */], np = 0;
    const Dav1dFrameContext *const f = t->f;
    const ptrdiff_t b4_stride = f->b4_stride;
    const refmvs *const r = &f->mvs[t->by * b4_stride + t->bx];

#define add_sample(dx, dy, sx, sy, rp) do { \
    pts[np][0][0] = 16 * (2 * dx + sx * bs(rp)[0]) - 8; \
    pts[np][0][1] = 16 * (2 * dy + sy * bs(rp)[1]) - 8; \
    pts[np][1][0] = pts[np][0][0] + (rp)->mv[0].x; \
    pts[np][1][1] = pts[np][0][1] + (rp)->mv[0].y; \
    np++; \
} while (0)

    // use masks[] to find the projectable motion vectors in the edges
    if ((unsigned) masks[0] == 1 && !(masks[1] >> 32)) {
        const int off = t->bx & (bs(&r[-b4_stride])[0] - 1);
        add_sample(-off, 0, 1, -1, &r[-b4_stride]);
    } else for (unsigned off = 0, xmask = masks[0]; np < 8 && xmask;) { // top
        const int tz = ctz(xmask);
        off += tz;
        add_sample(off, 0, 1, -1, &r[off - b4_stride]);
        xmask >>= tz + 1;
        off += 1;
    }
    if (np < 8 && masks[1] == 1) {
        const int off = t->by & (bs(&r[-1])[1] - 1);
        add_sample(0, -off, -1, 1, &r[-1 - off * b4_stride]);
    } else for (unsigned off = 0, ymask = masks[1]; np < 8 && ymask;) { // left
        const int tz = ctz(ymask);
        off += tz;
        add_sample(0, off, -1, 1, &r[off * b4_stride - 1]);
        ymask >>= tz + 1;
        off += 1;
    }
    if (np < 8 && masks[1] >> 32) // top/left
        add_sample(0, 0, -1, -1, &r[-(1 + b4_stride)]);
    if (np < 8 && masks[0] >> 32) // top/right
        add_sample(bw4, 0, 1, -1, &r[bw4 - b4_stride]);
    assert(np > 0 && np <= 8);
#undef bs

    // select according to motion vector difference against a threshold
    int mvd[8], ret = 0;
    const int thresh = 4 * iclip(imax(bw4, bh4), 4, 28);
    for (int i = 0; i < np; i++) {
        mvd[i] = labs(pts[i][1][0] - pts[i][0][0] - mv.x) +
                 labs(pts[i][1][1] - pts[i][0][1] - mv.y);
        if (mvd[i] > thresh)
            mvd[i] = -1;
        else
            ret++;
    }
    if (!ret) {
        ret = 1;
    } else for (int i = 0, j = np - 1, k = 0; k < np - ret; k++, i++, j--) {
        while (mvd[i] != -1) i++;
        while (mvd[j] == -1) j--;
        assert(i != j);
        if (i > j) break;
        // replace the discarded samples;
        mvd[i] = mvd[j];
        memcpy(pts[i], pts[j], sizeof(*pts));
    }

    if (!find_affine_int(pts, ret, bw4, bh4, mv, wmp, t->bx, t->by) &&
        !get_shear_params(wmp))
    {
        wmp->type = WM_TYPE_AFFINE;
    } else
        wmp->type = WM_TYPE_IDENTITY;
}

static inline int findoddzero(const uint8_t *buf, int len) {
    for (int n = 0; n < len; n++)
        if (!buf[n * 2]) return 1;
    return 0;
}

static void read_pal_plane(Dav1dTileContext *const t, Av1Block *const b,
                           const int pl, const int sz_ctx,
                           const int bx4, const int by4)
{
    Dav1dTileState *const ts = t->ts;
    const Dav1dFrameContext *const f = t->f;
    const int pal_sz = b->pal_sz[pl] = 2 + msac_decode_symbol_adapt(&ts->msac,
                                                 ts->cdf.m.pal_sz[pl][sz_ctx], 7);
    uint16_t cache[16], used_cache[8];
    int l_cache = pl ? t->pal_sz_uv[1][by4] : t->l.pal_sz[by4];
    int n_cache = 0;
    // don't reuse above palette outside SB64 boundaries
    int a_cache = by4 & 15 ? pl ? t->pal_sz_uv[0][bx4] : t->a->pal_sz[bx4] : 0;
    const uint16_t *l = t->al_pal[1][by4][pl], *a = t->al_pal[0][bx4][pl];

    // fill/sort cache
    while (l_cache && a_cache) {
        if (*l < *a) {
            if (!n_cache || cache[n_cache - 1] != *l)
                cache[n_cache++] = *l;
            l++;
            l_cache--;
        } else {
            if (*a == *l) {
                l++;
                l_cache--;
            }
            if (!n_cache || cache[n_cache - 1] != *a)
                cache[n_cache++] = *a;
            a++;
            a_cache--;
        }
    }
    if (l_cache) {
        do {
            if (!n_cache || cache[n_cache - 1] != *l)
                cache[n_cache++] = *l;
            l++;
        } while (--l_cache > 0);
    } else if (a_cache) {
        do {
            if (!n_cache || cache[n_cache - 1] != *a)
                cache[n_cache++] = *a;
            a++;
        } while (--a_cache > 0);
    }

    // find reused cache entries
    int i = 0;
    for (int n = 0; n < n_cache && i < pal_sz; n++)
        if (msac_decode_bool(&ts->msac, 128 << 7))
            used_cache[i++] = cache[n];
    const int n_used_cache = i;

    // parse new entries
    uint16_t *const pal = f->frame_thread.pass ?
        f->frame_thread.pal[((t->by >> 1) + (t->bx & 1)) * (f->b4_stride >> 1) +
                            ((t->bx >> 1) + (t->by & 1))][pl] : t->pal[pl];
    if (i < pal_sz) {
        int prev = pal[i++] = msac_decode_bools(&ts->msac, f->cur.p.p.bpc);

        if (i < pal_sz) {
            int bits = f->cur.p.p.bpc - 3 + msac_decode_bools(&ts->msac, 2);
            const int max = (1 << f->cur.p.p.bpc) - 1;

            do {
                const int delta = msac_decode_bools(&ts->msac, bits);
                prev = pal[i++] = imin(prev + delta + !pl, max);
                if (prev + !pl >= max) {
                    for (; i < pal_sz; i++)
                        pal[i] = pal[i - 1];
                    break;
                }
                bits = imin(bits, 1 + ulog2(max - prev - !pl));
            } while (i < pal_sz);
        }

        // merge cache+new entries
        int n = 0, m = n_used_cache;
        for (i = 0; i < pal_sz; i++) {
            if (n < n_used_cache && (m >= pal_sz || used_cache[n] <= pal[m])) {
                pal[i] = used_cache[n++];
            } else {
                assert(m < pal_sz);
                pal[i] = pal[m++];
            }
        }
    } else {
        memcpy(pal, used_cache, n_used_cache * sizeof(*used_cache));
    }

    if (DEBUG_BLOCK_INFO) {
        printf("Post-pal[pl=%d,sz=%d,cache_size=%d,used_cache=%d]: r=%d, cache=",
               pl, pal_sz, n_cache, n_used_cache, ts->msac.rng);
        for (int n = 0; n < n_cache; n++)
            printf("%c%02x", n ? ' ' : '[', cache[n]);
        printf("%s, pal=", n_cache ? "]" : "[]");
        for (int n = 0; n < pal_sz; n++)
            printf("%c%02x", n ? ' ' : '[', pal[n]);
        printf("]\n");
    }
}

static void read_pal_uv(Dav1dTileContext *const t, Av1Block *const b,
                        const int sz_ctx, const int bx4, const int by4)
{
    read_pal_plane(t, b, 1, sz_ctx, bx4, by4);

    // V pal coding
    Dav1dTileState *const ts = t->ts;
    const Dav1dFrameContext *const f = t->f;
    uint16_t *const pal = f->frame_thread.pass ?
        f->frame_thread.pal[((t->by >> 1) + (t->bx & 1)) * (f->b4_stride >> 1) +
                            ((t->bx >> 1) + (t->by & 1))][2] : t->pal[2];
    if (msac_decode_bool(&ts->msac, 128 << 7)) {
        const int bits = f->cur.p.p.bpc - 4 + msac_decode_bools(&ts->msac, 2);
        int prev = pal[0] = msac_decode_bools(&ts->msac, f->cur.p.p.bpc);
        const int max = (1 << f->cur.p.p.bpc) - 1;
        for (int i = 1; i < b->pal_sz[1]; i++) {
            int delta = msac_decode_bools(&ts->msac, bits);
            if (delta && msac_decode_bool(&ts->msac, 128 << 7)) delta = -delta;
            prev = pal[i] = (prev + delta) & max;
        }
    } else {
        for (int i = 0; i < b->pal_sz[1]; i++)
            pal[i] = msac_decode_bools(&ts->msac, f->cur.p.p.bpc);
    }
    if (DEBUG_BLOCK_INFO) {
        printf("Post-pal[pl=2]: r=%d ", ts->msac.rng);
        for (int n = 0; n < b->pal_sz[1]; n++)
            printf("%c%02x", n ? ' ' : '[', pal[n]);
        printf("]\n");
    }
}

// meant to be SIMD'able, so that theoretical complexity of this function
// times block size goes from w4*h4 to w4+h4-1
// a and b are previous two lines containing (a) top/left entries or (b)
// top/left entries, with a[0] being either the first top or first left entry,
// depending on top_offset being 1 or 0, and b being the first top/left entry
// for whichever has one. left_offset indicates whether the (len-1)th entry
// has a left neighbour.
// output is order[] and ctx for each member of this diagonal.
static void order_palette(const uint8_t *pal_idx, const ptrdiff_t stride,
                          const int i, const int first, const int last,
                          uint8_t (*const order)[8], uint8_t *const ctx)
{
    int have_top = i > first;

    pal_idx += first + (i - first) * stride;
    for (int j = first, n = 0; j >= last; have_top = 1, j--, n++, pal_idx += stride - 1) {
        const int have_left = j > 0;

        assert(have_left || have_top);

#define add(v_in) do { \
        const int v = v_in; \
        assert(v < 8U); \
        order[n][o_idx++] = v; \
        mask |= 1 << v; \
    } while (0)

        unsigned mask = 0;
        int o_idx = 0;
        if (!have_left) {
            ctx[n] = 0;
            add(pal_idx[-stride]);
        } else if (!have_top) {
            ctx[n] = 0;
            add(pal_idx[-1]);
        } else {
            const int l = pal_idx[-1], t = pal_idx[-stride], tl = pal_idx[-(stride + 1)];
            const int same_t_l = t == l;
            const int same_t_tl = t == tl;
            const int same_l_tl = l == tl;
            const int same_all = same_t_l & same_t_tl & same_l_tl;

            if (same_all) {
                ctx[n] = 4;
                add(t);
            } else if (same_t_l) {
                ctx[n] = 3;
                add(t);
                add(tl);
            } else if (same_t_tl | same_l_tl) {
                ctx[n] = 2;
                add(tl);
                add(same_t_tl ? l : t);
            } else {
                ctx[n] = 1;
                add(imin(t, l));
                add(imax(t, l));
                add(tl);
            }
        }
        for (unsigned m = 1, bit = 0; m < 0x100; m <<= 1, bit++)
            if (!(mask & m))
                order[n][o_idx++] = bit;
        assert(o_idx == 8);
#undef add
    }
}

static void read_pal_indices(Dav1dTileContext *const t,
                             uint8_t *const pal_idx,
                             const Av1Block *const b, const int pl,
                             const int w4, const int h4,
                             const int bw4, const int bh4)
{
    Dav1dTileState *const ts = t->ts;
    const ptrdiff_t stride = bw4 * 4;
    pal_idx[0] = msac_decode_uniform(&ts->msac, b->pal_sz[pl]);
    uint16_t (*const color_map_cdf)[8 + 1] =
        ts->cdf.m.color_map[pl][b->pal_sz[pl] - 2];
    for (int i = 1; i < 4 * (w4 + h4) - 1; i++) {
        // top/left-to-bottom/right diagonals ("wave-front")
        uint8_t order[64][8], ctx[64];
        const int first = imin(i, w4 * 4 - 1);
        const int last = imax(0, i - h4 * 4 + 1);
        order_palette(pal_idx, stride, i, first, last, order, ctx);
        for (int j = first, m = 0; j >= last; j--, m++) {
            const int color_idx =
                msac_decode_symbol_adapt(&ts->msac, color_map_cdf[ctx[m]],
                                         b->pal_sz[pl]);
            pal_idx[(i - j) * stride + j] = order[m][color_idx];
        }
    }
    // fill invisible edges
    if (bw4 > w4)
        for (int y = 0; y < 4 * h4; y++)
            memset(&pal_idx[y * stride + 4 * w4],
                   pal_idx[y * stride + 4 * w4 - 1], 4 * (bw4 - w4));
    if (h4 < bh4) {
        const uint8_t *const src = &pal_idx[stride * (4 * h4 - 1)];
        for (int y = h4 * 4; y < bh4 * 4; y++)
            memcpy(&pal_idx[y * stride], src, bw4 * 4);
    }
}

static void read_vartx_tree(Dav1dTileContext *const t,
                            Av1Block *const b, const enum BlockSize bs,
                            const int bx4, const int by4)
{
    const Dav1dFrameContext *const f = t->f;
    const uint8_t *const b_dim = av1_block_dimensions[bs];
    const int bw4 = b_dim[0], bh4 = b_dim[1];

    // var-tx tree coding
    b->tx_split[0] = b->tx_split[1] = 0;
    b->max_ytx = av1_max_txfm_size_for_bs[bs][0];
    if (f->frame_hdr.segmentation.lossless[b->seg_id] ||
        b->max_ytx == TX_4X4)
    {
        b->max_ytx = b->uvtx = TX_4X4;
        if (f->frame_hdr.txfm_mode == TX_SWITCHABLE) {
            memset(&t->a->tx[bx4], TX_4X4, bw4);
            memset(&t->l.tx[by4], TX_4X4, bh4);
        }
    } else if (f->frame_hdr.txfm_mode != TX_SWITCHABLE || b->skip) {
        if (f->frame_hdr.txfm_mode == TX_SWITCHABLE) {
            memset(&t->a->tx[bx4], b_dim[2], bw4);
            memset(&t->l.tx[by4], b_dim[3], bh4);
        } else {
            assert(f->frame_hdr.txfm_mode == TX_LARGEST);
        }
        b->uvtx = av1_max_txfm_size_for_bs[bs][f->cur.p.p.layout];
    } else {
        assert(imin(bw4, bh4) <= 16 || b->max_ytx == TX_64X64);
        int y, x, y_off, x_off;
        const TxfmInfo *const ytx = &av1_txfm_dimensions[b->max_ytx];
        for (y = 0, y_off = 0; y < bh4; y += ytx->h, y_off++) {
            for (x = 0, x_off = 0; x < bw4; x += ytx->w, x_off++) {
                read_tx_tree(t, b->max_ytx, 0, b->tx_split, x_off, y_off);
                // contexts are updated inside read_tx_tree()
                t->bx += ytx->w;
            }
            t->bx -= x;
            t->by += ytx->h;
        }
        t->by -= y;
        if (DEBUG_BLOCK_INFO)
            printf("Post-vartxtree[%x/%x]: r=%d\n",
                   b->tx_split[0], b->tx_split[1], t->ts->msac.rng);
        b->uvtx = av1_max_txfm_size_for_bs[bs][f->cur.p.p.layout];
    }
}

static inline unsigned get_prev_frame_segid(const Dav1dFrameContext *const f,
                                            const int by, const int bx,
                                            const int w4, int h4,
                                            const uint8_t *ref_seg_map,
                                            const ptrdiff_t stride)
{
    unsigned seg_id = 8;

    assert(f->frame_hdr.primary_ref_frame != PRIMARY_REF_NONE);
    dav1d_thread_picture_wait(&f->refp[f->frame_hdr.primary_ref_frame],
                              (by + h4) * 4, PLANE_TYPE_BLOCK);

    ref_seg_map += by * stride + bx;
    do {
        for (int x = 0; x < w4; x++)
            seg_id = imin(seg_id, ref_seg_map[x]);
        ref_seg_map += stride;
    } while (--h4 > 0);
    assert(seg_id < 8);

    return seg_id;
}

static void decode_b(Dav1dTileContext *const t,
                     const enum BlockLevel bl,
                     const enum BlockSize bs,
                     const enum BlockPartition bp,
                     const enum EdgeFlags intra_edge_flags)
{
    Dav1dTileState *const ts = t->ts;
    const Dav1dFrameContext *const f = t->f;
    Av1Block b_mem, *const b = f->frame_thread.pass ?
        &f->frame_thread.b[t->by * f->b4_stride + t->bx] : &b_mem;
    const uint8_t *const b_dim = av1_block_dimensions[bs];
    const int bx4 = t->bx & 31, by4 = t->by & 31;
    const int ss_ver = f->cur.p.p.layout == DAV1D_PIXEL_LAYOUT_I420;
    const int ss_hor = f->cur.p.p.layout != DAV1D_PIXEL_LAYOUT_I444;
    const int cbx4 = bx4 >> ss_hor, cby4 = by4 >> ss_ver;
    const int bw4 = b_dim[0], bh4 = b_dim[1];
    const int w4 = imin(bw4, f->bw - t->bx), h4 = imin(bh4, f->bh - t->by);
    const int cbw4 = (bw4 + ss_hor) >> ss_hor, cbh4 = (bh4 + ss_ver) >> ss_ver;
    const int have_left = t->bx > ts->tiling.col_start;
    const int have_top = t->by > ts->tiling.row_start;
    const int has_chroma = f->seq_hdr.layout != DAV1D_PIXEL_LAYOUT_I400 &&
                           (bw4 > ss_hor || t->bx & 1) &&
                           (bh4 > ss_ver || t->by & 1);

    if (f->frame_thread.pass == 2) {
        if (b->intra) {
            f->bd_fn.recon_b_intra(t, bs, intra_edge_flags, b);

            if (has_chroma) {
                memset(&t->l.uvmode[cby4], b->uv_mode, cbh4);
                memset(&t->a->uvmode[cbx4], b->uv_mode, cbw4);
            }
            const enum IntraPredMode y_mode_nofilt =
                b->y_mode == FILTER_PRED ? DC_PRED : b->y_mode;
            memset(&t->l.mode[by4], y_mode_nofilt, bh4);
            memset(&t->a->mode[bx4], y_mode_nofilt, bw4);
        } else {
            if (b->comp_type == COMP_INTER_NONE && b->motion_mode == MM_WARP) {
                uint64_t mask[2] = { 0, 0 };
                find_matching_ref(t, intra_edge_flags, bw4, bh4, w4, h4,
                                  have_left, have_top, b->ref[0], mask);
                derive_warpmv(t, bw4, bh4, mask, b->mv[0], &t->warpmv);
            }
            f->bd_fn.recon_b_inter(t, bs, b);

            const uint8_t *const filter = eve_av1_filter_dir[b->filter2d];
            memset(&t->l.filter[0][by4], filter[0], bh4);
            memset(&t->a->filter[0][bx4], filter[0], bw4);
            memset(&t->l.filter[1][by4], filter[1], bh4);
            memset(&t->a->filter[1][bx4], filter[1], bw4);
            if (has_chroma) {
                memset(&t->l.uvmode[cby4], DC_PRED, cbh4);
                memset(&t->a->uvmode[cbx4], DC_PRED, cbw4);
            }
        }
        memset(&t->l.intra[by4], b->intra, bh4);
        memset(&t->a->intra[bx4], b->intra, bw4);
        return;
    }

    const int cw4 = (w4 + ss_hor) >> ss_hor, ch4 = (h4 + ss_ver) >> ss_ver;

    b->bl = bl;
    b->bp = bp;
    b->bs = bs;

    // skip_mode
    if (f->frame_hdr.skip_mode_enabled && imin(bw4, bh4) > 1) {
        const int smctx = t->a->skip_mode[bx4] + t->l.skip_mode[by4];
        b->skip_mode = msac_decode_bool_adapt(&ts->msac,
                                              ts->cdf.m.skip_mode[smctx]);
        if (DEBUG_BLOCK_INFO)
            printf("Post-skipmode[%d]: r=%d\n", b->skip_mode, ts->msac.rng);
    } else {
        b->skip_mode = 0;
    }

    // segment_id (if seg_feature for skip/ref/gmv is enabled)
    int seg_pred = 0;
    if (f->frame_hdr.segmentation.enabled) {
        if (!f->frame_hdr.segmentation.update_map) {
            b->seg_id = f->prev_segmap ?
                        get_prev_frame_segid(f, t->by, t->bx, w4, h4,
                                             f->prev_segmap, f->b4_stride) : 0;
        } else if (f->frame_hdr.segmentation.seg_data.preskip) {
            if (f->frame_hdr.segmentation.temporal &&
                (seg_pred = msac_decode_bool_adapt(&ts->msac,
                                       ts->cdf.m.seg_pred[t->a->seg_pred[bx4] +
                                                          t->l.seg_pred[by4]])))
            {
                // temporal predicted seg_id
                b->seg_id = f->prev_segmap ?
                            get_prev_frame_segid(f, t->by, t->bx, w4, h4,
                                                 f->prev_segmap, f->b4_stride) : 0;
            } else {
                int seg_ctx;
                const unsigned pred_seg_id =
                    get_cur_frame_segid(t->by, t->bx, have_top, have_left,
                                        &seg_ctx, f->cur_segmap, f->b4_stride);
                const unsigned diff = msac_decode_symbol_adapt(&ts->msac,
                                                   ts->cdf.m.seg_id[seg_ctx],
                                                   NUM_SEGMENTS);
                const unsigned last_active_seg_id =
                    f->frame_hdr.segmentation.seg_data.last_active_segid;
                b->seg_id = av1_neg_deinterleave(diff, pred_seg_id,
                                                 last_active_seg_id + 1);
                if (b->seg_id > last_active_seg_id) b->seg_id = 0; // error?
            }

            if (DEBUG_BLOCK_INFO)
                printf("Post-segid[preskip;%d]: r=%d\n",
                       b->seg_id, ts->msac.rng);
        }
    } else {
        b->seg_id = 0;
    }

    // skip
    const int sctx = t->a->skip[bx4] + t->l.skip[by4];
    b->skip = b->skip_mode ? 1 :
              msac_decode_bool_adapt(&ts->msac, ts->cdf.m.skip[sctx]);
    if (DEBUG_BLOCK_INFO)
        printf("Post-skip[%d]: r=%d\n", b->skip, ts->msac.rng);

    // segment_id
    if (f->frame_hdr.segmentation.enabled &&
        f->frame_hdr.segmentation.update_map &&
        !f->frame_hdr.segmentation.seg_data.preskip)
    {
        if (!b->skip && f->frame_hdr.segmentation.temporal &&
            (seg_pred = msac_decode_bool_adapt(&ts->msac,
                                   ts->cdf.m.seg_pred[t->a->seg_pred[bx4] +
                                                      t->l.seg_pred[by4]])))
        {
            // temporal predicted seg_id
            b->seg_id = f->prev_segmap ?
                        get_prev_frame_segid(f, t->by, t->bx, w4, h4,
                                             f->prev_segmap, f->b4_stride) : 0;
        } else {
            int seg_ctx;
            const unsigned pred_seg_id =
                get_cur_frame_segid(t->by, t->bx, have_top, have_left,
                                    &seg_ctx, f->cur_segmap, f->b4_stride);
            if (b->skip) {
                b->seg_id = pred_seg_id;
            } else {
                const unsigned diff = msac_decode_symbol_adapt(&ts->msac,
                                                   ts->cdf.m.seg_id[seg_ctx],
                                                   NUM_SEGMENTS);
                const unsigned last_active_seg_id =
                    f->frame_hdr.segmentation.seg_data.last_active_segid;
                b->seg_id = av1_neg_deinterleave(diff, pred_seg_id,
                                                 last_active_seg_id + 1);
                if (b->seg_id > last_active_seg_id) b->seg_id = 0; // error?
            }
        }

        if (DEBUG_BLOCK_INFO)
            printf("Post-segid[postskip;%d]: r=%d\n",
                   b->seg_id, ts->msac.rng);
    }

    // cdef index
    if (!b->skip) {
        const int idx = f->seq_hdr.sb128 ? ((t->bx & 16) >> 4) +
                                           ((t->by & 16) >> 3) : 0;
        if (t->cur_sb_cdef_idx_ptr[idx] == -1) {
            const int v = msac_decode_bools(&ts->msac, f->frame_hdr.cdef.n_bits);
            t->cur_sb_cdef_idx_ptr[idx] = v;
            if (bw4 > 16) t->cur_sb_cdef_idx_ptr[idx + 1] = v;
            if (bh4 > 16) t->cur_sb_cdef_idx_ptr[idx + 2] = v;
            if (bw4 == 32 && bh4 == 32) t->cur_sb_cdef_idx_ptr[idx + 3] = v;

            if (DEBUG_BLOCK_INFO)
                printf("Post-cdef_idx[%d]: r=%d\n",
                        *t->cur_sb_cdef_idx_ptr, ts->msac.rng);
        }
    }

    // delta-q/lf
    if (!(t->bx & (31 >> !f->seq_hdr.sb128)) &&
        !(t->by & (31 >> !f->seq_hdr.sb128)))
    {
        const int prev_qidx = ts->last_qidx;
        const int have_delta_q = f->frame_hdr.delta.q.present &&
            (bs != (f->seq_hdr.sb128 ? BS_128x128 : BS_64x64) || !b->skip);
        if (have_delta_q) {
            int delta_q = msac_decode_symbol_adapt(&ts->msac, ts->cdf.m.delta_q, 4);
            if (delta_q == 3) {
                const int n_bits = 1 + msac_decode_bools(&ts->msac, 3);
                delta_q = msac_decode_bools(&ts->msac, n_bits) + 1 + (1 << n_bits);
            }
            if (delta_q) {
                if (msac_decode_bool(&ts->msac, 128 << 7)) delta_q = -delta_q;
                delta_q *= 1 << f->frame_hdr.delta.q.res_log2;
            }
            ts->last_qidx = iclip(ts->last_qidx + delta_q, 1, 255);
            if (have_delta_q && DEBUG_BLOCK_INFO)
                printf("Post-delta_q[%d->%d]: r=%d\n",
                       delta_q, ts->last_qidx, ts->msac.rng);
        }
        if (ts->last_qidx == f->frame_hdr.quant.yac) {
            // assign frame-wide q values to this sb
            ts->dq = f->dq;
        } else if (ts->last_qidx != prev_qidx) {
            // find sb-specific quant parameters
            init_quant_tables(&f->seq_hdr, &f->frame_hdr, ts->last_qidx, ts->dqmem);
            ts->dq = ts->dqmem;
        }

        // delta_lf
        int8_t prev_delta_lf[4];
        memcpy(prev_delta_lf, ts->last_delta_lf, 4);
        if (have_delta_q && f->frame_hdr.delta.lf.present) {
            const int n_lfs = f->frame_hdr.delta.lf.multi ?
                f->seq_hdr.layout != DAV1D_PIXEL_LAYOUT_I400 ? 4 : 2 : 1;

            for (int i = 0; i < n_lfs; i++) {
                int delta_lf = msac_decode_symbol_adapt(&ts->msac,
                                ts->cdf.m.delta_lf[i + f->frame_hdr.delta.lf.multi], 4);
                if (delta_lf == 3) {
                    const int n_bits = 1 + msac_decode_bools(&ts->msac, 3);
                    delta_lf = msac_decode_bools(&ts->msac, n_bits) + 1 + (1 << n_bits);
                }
                if (delta_lf) {
                    if (msac_decode_bool(&ts->msac, 128 << 7)) delta_lf = -delta_lf;
                    delta_lf *= 1 << f->frame_hdr.delta.lf.res_log2;
                }
                ts->last_delta_lf[i] = iclip(ts->last_delta_lf[i] + delta_lf, -63, 63);
                if (have_delta_q && DEBUG_BLOCK_INFO)
                    printf("Post-delta_lf[%d:%d]: r=%d\n", i, delta_lf, ts->msac.rng);
            }
        }
        if (!memcmp(ts->last_delta_lf, (int8_t[4]) { 0, 0, 0, 0 }, 4)) {
            // assign frame-wide lf values to this sb
            ts->lflvl = f->lf.lvl;
        } else if (memcmp(ts->last_delta_lf, prev_delta_lf, 4)) {
            // find sb-specific lf lvl parameters
            dav1d_calc_lf_values(ts->lflvlmem, &f->frame_hdr, ts->last_delta_lf);
            ts->lflvl = ts->lflvlmem;
        }
    }

    if (b->skip_mode) {
        b->intra = 0;
    } else if (f->frame_hdr.frame_type & 1) {
        const int ictx = get_intra_ctx(t->a, &t->l, by4, bx4,
                                       have_top, have_left);
        b->intra = !msac_decode_bool_adapt(&ts->msac, ts->cdf.m.intra[ictx]);
        if (DEBUG_BLOCK_INFO)
            printf("Post-intra[%d]: r=%d\n", b->intra, ts->msac.rng);
    } else if (f->frame_hdr.allow_intrabc) {
        b->intra = !msac_decode_bool_adapt(&ts->msac, ts->cdf.m.intrabc);
        if (DEBUG_BLOCK_INFO)
            printf("Post-intrabcflag[%d]: r=%d\n", b->intra, ts->msac.rng);
    } else {
        b->intra = 1;
    }

    // intra/inter-specific stuff
    if (b->intra) {
        uint16_t *const ymode_cdf = f->frame_hdr.frame_type & 1 ?
            ts->cdf.m.y_mode[av1_ymode_size_context[bs]] :
            ts->cdf.kfym[intra_mode_context[t->a->mode[bx4]]]
                        [intra_mode_context[t->l.mode[by4]]];
        b->y_mode = msac_decode_symbol_adapt(&ts->msac, ymode_cdf,
                                              N_INTRA_PRED_MODES);
        if (DEBUG_BLOCK_INFO)
            printf("Post-ymode[%d]: r=%d\n", b->y_mode, ts->msac.rng);

        // angle delta
        if (b_dim[2] + b_dim[3] >= 2 && b->y_mode >= VERT_PRED &&
            b->y_mode <= VERT_LEFT_PRED)
        {
            uint16_t *const acdf = ts->cdf.m.angle_delta[b->y_mode - VERT_PRED];
            const int angle = msac_decode_symbol_adapt(&ts->msac, acdf, 7);
            b->y_angle = angle - 3;
        } else {
            b->y_angle = 0;
        }

        if (has_chroma) {
            const int cfl_allowed = !!(cfl_allowed_mask & (1 << bs));
            uint16_t *const uvmode_cdf = ts->cdf.m.uv_mode[cfl_allowed][b->y_mode];
            b->uv_mode = msac_decode_symbol_adapt(&ts->msac, uvmode_cdf,
                                         N_UV_INTRA_PRED_MODES - !cfl_allowed);
            if (DEBUG_BLOCK_INFO)
                printf("Post-uvmode[%d]: r=%d\n", b->uv_mode, ts->msac.rng);

            if (b->uv_mode == CFL_PRED) {
#define SIGN(a) (!!(a) + ((a) > 0))
                const int sign =
                    msac_decode_symbol_adapt(&ts->msac, ts->cdf.m.cfl_sign, 8) + 1;
                const int sign_u = sign * 0x56 >> 8, sign_v = sign - sign_u * 3;
                assert(sign_u == sign / 3);
                if (sign_u) {
                    const int ctx = (sign_u == 2) * 3 + sign_v;
                    b->cfl_alpha[0] = msac_decode_symbol_adapt(&ts->msac,
                                            ts->cdf.m.cfl_alpha[ctx], 16) + 1;
                    if (sign_u == 1) b->cfl_alpha[0] = -b->cfl_alpha[0];
                } else {
                    b->cfl_alpha[0] = 0;
                }
                if (sign_v) {
                    const int ctx = (sign_v == 2) * 3 + sign_u;
                    b->cfl_alpha[1] = msac_decode_symbol_adapt(&ts->msac,
                                            ts->cdf.m.cfl_alpha[ctx], 16) + 1;
                    if (sign_v == 1) b->cfl_alpha[1] = -b->cfl_alpha[1];
                } else {
                    b->cfl_alpha[1] = 0;
                }
#undef SIGN
                if (DEBUG_BLOCK_INFO)
                    printf("Post-uvalphas[%d/%d]: r=%d\n",
                           b->cfl_alpha[0], b->cfl_alpha[1], ts->msac.rng);
            } else if (b_dim[2] + b_dim[3] >= 2 && b->uv_mode >= VERT_PRED &&
                       b->uv_mode <= VERT_LEFT_PRED)
            {
                uint16_t *const acdf = ts->cdf.m.angle_delta[b->uv_mode - VERT_PRED];
                const int angle = msac_decode_symbol_adapt(&ts->msac, acdf, 7);
                b->uv_angle = angle - 3;
            } else {
                b->uv_angle = 0;
            }
        }

        b->pal_sz[0] = b->pal_sz[1] = 0;
        if (f->frame_hdr.allow_screen_content_tools &&
            imax(bw4, bh4) <= 16 && bw4 + bh4 >= 4)
        {
            const int sz_ctx = b_dim[2] + b_dim[3] - 2;
            if (b->y_mode == DC_PRED) {
                const int pal_ctx = (t->a->pal_sz[bx4] > 0) + (t->l.pal_sz[by4] > 0);
                const int use_y_pal =
                    msac_decode_bool_adapt(&ts->msac, ts->cdf.m.pal_y[sz_ctx][pal_ctx]);
                if (DEBUG_BLOCK_INFO)
                    printf("Post-y_pal[%d]: r=%d\n", use_y_pal, ts->msac.rng);
                if (use_y_pal)
                    read_pal_plane(t, b, 0, sz_ctx, bx4, by4);
            }

            if (has_chroma && b->uv_mode == DC_PRED) {
                const int pal_ctx = b->pal_sz[0] > 0;
                const int use_uv_pal =
                    msac_decode_bool_adapt(&ts->msac, ts->cdf.m.pal_uv[pal_ctx]);
                if (DEBUG_BLOCK_INFO)
                    printf("Post-uv_pal[%d]: r=%d\n", use_uv_pal, ts->msac.rng);
                if (use_uv_pal) // see aomedia bug 2183 for why we use luma coordinates
                    read_pal_uv(t, b, sz_ctx, bx4, by4);
            }
        }

        if (b->y_mode == DC_PRED && !b->pal_sz[0] &&
            imax(b_dim[2], b_dim[3]) <= 3 && f->seq_hdr.filter_intra)
        {
            const int is_filter = msac_decode_bool_adapt(&ts->msac,
                                            ts->cdf.m.use_filter_intra[bs]);
            if (is_filter) {
                b->y_mode = FILTER_PRED;
                b->y_angle = msac_decode_symbol_adapt(&ts->msac,
                                                  ts->cdf.m.filter_intra, 5);
            }
            if (DEBUG_BLOCK_INFO)
                printf("Post-filterintramode[%d/%d]: r=%d\n",
                       b->y_mode, b->y_angle, ts->msac.rng);
        }

        if (b->pal_sz[0]) {
            uint8_t *pal_idx;
            if (f->frame_thread.pass) {
                pal_idx = ts->frame_thread.pal_idx;
                ts->frame_thread.pal_idx += bw4 * bh4 * 16;
            } else
                pal_idx = t->scratch.pal_idx;
            read_pal_indices(t, pal_idx, b, 0, w4, h4, bw4, bh4);
            if (DEBUG_BLOCK_INFO)
                printf("Post-y-pal-indices: r=%d\n", ts->msac.rng);
        }

        if (has_chroma && b->pal_sz[1]) {
            uint8_t *pal_idx;
            if (f->frame_thread.pass) {
                pal_idx = ts->frame_thread.pal_idx;
                ts->frame_thread.pal_idx += cbw4 * cbh4 * 16;
            } else
                pal_idx = &t->scratch.pal_idx[bw4 * bh4 * 16];
            read_pal_indices(t, pal_idx, b, 1, cw4, ch4, cbw4, cbh4);
            if (DEBUG_BLOCK_INFO)
                printf("Post-uv-pal-indices: r=%d\n", ts->msac.rng);
        }

        const TxfmInfo *t_dim;
        if (f->frame_hdr.segmentation.lossless[b->seg_id]) {
            b->tx = b->uvtx = (int) TX_4X4;
            t_dim = &av1_txfm_dimensions[TX_4X4];
        } else {
            b->tx = av1_max_txfm_size_for_bs[bs][0];
            b->uvtx = av1_max_txfm_size_for_bs[bs][f->cur.p.p.layout];
            t_dim = &av1_txfm_dimensions[b->tx];
            if (f->frame_hdr.txfm_mode == TX_SWITCHABLE && t_dim->max > TX_4X4) {
                const int tctx = get_tx_ctx(t->a, &t->l, t_dim, by4, bx4);
                uint16_t *const tx_cdf = ts->cdf.m.txsz[t_dim->max - 1][tctx];
                int depth = msac_decode_symbol_adapt(&ts->msac, tx_cdf,
                                                     imin(t_dim->max + 1, 3));

                while (depth--) {
                    b->tx = t_dim->sub;
                    t_dim = &av1_txfm_dimensions[b->tx];
                }
            }
            if (DEBUG_BLOCK_INFO)
                printf("Post-tx[%d]: r=%d\n", b->tx, ts->msac.rng);
        }

        // reconstruction
        if (f->frame_thread.pass == 1) {
            f->bd_fn.read_coef_blocks(t, bs, b);
        } else {
            f->bd_fn.recon_b_intra(t, bs, intra_edge_flags, b);
        }

        dav1d_create_lf_mask_intra(t->lf_mask, f->lf.level, f->b4_stride,
                                   &f->frame_hdr, (const uint8_t (*)[8][2])
                                   &ts->lflvl[b->seg_id][0][0][0],
                                   t->bx, t->by, f->bw, f->bh, bs,
                                   b->tx, b->uvtx, f->cur.p.p.layout,
                                   &t->a->tx_lpf_y[bx4], &t->l.tx_lpf_y[by4],
                                   has_chroma ? &t->a->tx_lpf_uv[cbx4] : NULL,
                                   has_chroma ? &t->l.tx_lpf_uv[cby4] : NULL);

        // update contexts
        memset(&t->a->tx_intra[bx4], t_dim->lw, bw4);
        memset(&t->l.tx_intra[by4], t_dim->lh, bh4);
        const enum IntraPredMode y_mode_nofilt =
            b->y_mode == FILTER_PRED ? DC_PRED : b->y_mode;
        memset(&t->l.mode[by4], y_mode_nofilt, bh4);
        memset(&t->a->mode[bx4], y_mode_nofilt, bw4);
        memset(&t->l.pal_sz[by4], b->pal_sz[0], bh4);
        memset(&t->a->pal_sz[bx4], b->pal_sz[0], bw4);
        if (b->pal_sz[0]) {
            uint16_t *const pal = f->frame_thread.pass ?
                f->frame_thread.pal[((t->by >> 1) + (t->bx & 1)) * (f->b4_stride >> 1) +
                                    ((t->bx >> 1) + (t->by & 1))][0] : t->pal[0];
            for (int x = 0; x < bw4; x++)
                memcpy(t->al_pal[0][bx4 + x][0], pal, 16);
            for (int y = 0; y < bh4; y++)
                memcpy(t->al_pal[1][by4 + y][0], pal, 16);
        }
        if (has_chroma) {
            memset(&t->l.uvmode[cby4], b->uv_mode, cbh4);
            memset(&t->a->uvmode[cbx4], b->uv_mode, cbw4);
            // see aomedia bug 2183 for why we use luma coordinates here
            memset(&t->pal_sz_uv[1][by4], b->pal_sz[1], bh4);
            memset(&t->pal_sz_uv[0][bx4], b->pal_sz[1], bw4);
            if (b->pal_sz[1]) for (int pl = 1; pl < 3; pl++) {
                uint16_t *const pal = f->frame_thread.pass ?
                    f->frame_thread.pal[((t->by >> 1) + (t->bx & 1)) * (f->b4_stride >> 1) +
                                        ((t->bx >> 1) + (t->by & 1))][pl] : t->pal[pl];
                // see aomedia bug 2183 for why we use luma coordinates here
                for (int x = 0; x < bw4; x++)
                    memcpy(t->al_pal[0][bx4 + x][pl], pal, 16);
                for (int y = 0; y < bh4; y++)
                    memcpy(t->al_pal[1][by4 + y][pl], pal, 16);
            }
        } else { // see aomedia bug 2183 for why we reset this
            memset(&t->pal_sz_uv[1][by4], 0, bh4);
            memset(&t->pal_sz_uv[0][bx4], 0, bw4);
        }
        if ((f->frame_hdr.frame_type & 1) || f->frame_hdr.allow_intrabc) {
            memset(&t->a->tx[bx4], t_dim->lw, bw4);
            memset(&t->l.tx[by4], t_dim->lh, bh4);
            splat_intraref(f->mvs, f->b4_stride, t->by, t->bx, bs,
                           y_mode_nofilt);
        }
        if (f->frame_hdr.frame_type & 1) {
            memset(&t->l.comp_type[by4], COMP_INTER_NONE, bh4);
            memset(&t->a->comp_type[bx4], COMP_INTER_NONE, bw4);
            memset(&t->l.ref[0][by4], -1, bh4);
            memset(&t->a->ref[0][bx4], -1, bw4);
            memset(&t->l.ref[1][by4], -1, bh4);
            memset(&t->a->ref[1][bx4], -1, bw4);
            memset(&t->l.filter[0][by4], N_SWITCHABLE_FILTERS, bh4);
            memset(&t->a->filter[0][bx4], N_SWITCHABLE_FILTERS, bw4);
            memset(&t->l.filter[1][by4], N_SWITCHABLE_FILTERS, bh4);
            memset(&t->a->filter[1][bx4], N_SWITCHABLE_FILTERS, bw4);
        }
    } else if (!(f->frame_hdr.frame_type & 1)) {
        // intra block copy
        candidate_mv mvstack[8];
        int n_mvs;
        mv mvlist[2][2];
        av1_find_ref_mvs(mvstack, &n_mvs, mvlist, NULL,
                         (int[2]) { -1, -1 }, f->bw, f->bh,
                         bs, bp, t->by, t->bx, ts->tiling.col_start,
                         ts->tiling.col_end, ts->tiling.row_start,
                         ts->tiling.row_end, f->libaom_cm);

        if (mvlist[0][0].y | mvlist[0][0].x)
            b->mv[0] = mvlist[0][0];
        else if (mvlist[0][1].y | mvlist[0][1].x)
            b->mv[0] = mvlist[0][1];
        else {
            if (t->by - (16 << f->seq_hdr.sb128) < ts->tiling.row_start) {
                b->mv[0].y = 0;
                b->mv[0].x = -(512 << f->seq_hdr.sb128) - 2048;
            } else {
                b->mv[0].y = -(512 << f->seq_hdr.sb128);
                b->mv[0].x = 0;
            }
        }

        const struct mv ref = b->mv[0];
        read_mv_residual(t, &b->mv[0], &ts->cdf.dmv, 0);
        if (DEBUG_BLOCK_INFO)
            printf("Post-dmv[%d/%d,ref=%d/%d|%d/%d]: r=%d\n",
                   b->mv[0].y, b->mv[0].x, ref.y, ref.x,
                   mvlist[0][0].y, mvlist[0][0].x, ts->msac.rng);
        read_vartx_tree(t, b, bs, bx4, by4);

        // reconstruction
        if (f->frame_thread.pass == 1) {
            f->bd_fn.read_coef_blocks(t, bs, b);
        } else {
            f->bd_fn.recon_b_inter(t, bs, b);
        }

        splat_intrabc_mv(f->mvs, f->b4_stride, t->by, t->bx, bs, b->mv[0]);

        memset(&t->a->tx_intra[bx4], b_dim[2], bw4);
        memset(&t->l.tx_intra[by4], b_dim[3], bh4);
        memset(&t->l.mode[by4], DC_PRED, bh4);
        memset(&t->a->mode[bx4], DC_PRED, bw4);
        memset(&t->l.pal_sz[by4], 0, bh4);
        memset(&t->a->pal_sz[bx4], 0, bw4);
        // see aomedia bug 2183 for why this is outside if (has_chroma)
        memset(&t->pal_sz_uv[1][by4], 0, bh4);
        memset(&t->pal_sz_uv[0][bx4], 0, bw4);
        if (has_chroma) {
            memset(&t->l.uvmode[cby4], DC_PRED, cbh4);
            memset(&t->a->uvmode[cbx4], DC_PRED, cbw4);
        }
    } else {
        // inter-specific mode/mv coding
        int is_comp, has_subpel_filter;

        if (b->skip_mode) {
            is_comp = 1;
        } else if (f->frame_hdr.switchable_comp_refs && imin(bw4, bh4) > 1) {
            const int ctx = get_comp_ctx(t->a, &t->l, by4, bx4,
                                         have_top, have_left);
            is_comp = msac_decode_bool_adapt(&ts->msac, ts->cdf.m.comp[ctx]);
            if (DEBUG_BLOCK_INFO)
                printf("Post-compflag[%d]: r=%d\n", is_comp, ts->msac.rng);
        } else {
            is_comp = 0;
        }

        if (b->skip_mode) {
            b->ref[0] = f->frame_hdr.skip_mode_refs[0];
            b->ref[1] = f->frame_hdr.skip_mode_refs[1];
            b->comp_type = COMP_INTER_AVG;
            b->inter_mode = NEARESTMV_NEARESTMV;
            b->drl_idx = 0;
            has_subpel_filter = 0;

            candidate_mv mvstack[8];
            int n_mvs, ctx;
            mv mvlist[2][2];
            av1_find_ref_mvs(mvstack, &n_mvs, mvlist, &ctx,
                             (int[2]) { b->ref[0], b->ref[1] }, f->bw, f->bh,
                             bs, bp, t->by, t->bx, ts->tiling.col_start,
                             ts->tiling.col_end, ts->tiling.row_start,
                             ts->tiling.row_end, f->libaom_cm);

            b->mv[0] = mvstack[0].this_mv;
            b->mv[1] = mvstack[0].comp_mv;
            if (!f->frame_hdr.hp) {
                unset_hp_bit(&b->mv[0]);
                unset_hp_bit(&b->mv[1]);
            }
            if (DEBUG_BLOCK_INFO)
                printf("Post-skipmodeblock[mv=1:y=%d,x=%d,2:y=%d,x=%d,refs=%d+%d\n",
                       b->mv[0].y, b->mv[0].x, b->mv[1].y, b->mv[1].x,
                       b->ref[0], b->ref[1]);
        } else if (is_comp) {
            const int dir_ctx = get_comp_dir_ctx(t->a, &t->l, by4, bx4,
                                                 have_top, have_left);
            if (msac_decode_bool_adapt(&ts->msac, ts->cdf.m.comp_dir[dir_ctx])) {
                // bidir - first reference (fw)
                const int ctx1 = av1_get_fwd_ref_ctx(t->a, &t->l, by4, bx4,
                                                     have_top, have_left);
                if (msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.comp_fwd_ref[0][ctx1]))
                {
                    const int ctx2 = av1_get_fwd_ref_2_ctx(t->a, &t->l, by4, bx4,
                                                           have_top, have_left);
                    b->ref[0] = 2 + msac_decode_bool_adapt(&ts->msac,
                                            ts->cdf.m.comp_fwd_ref[2][ctx2]);
                } else {
                    const int ctx2 = av1_get_fwd_ref_1_ctx(t->a, &t->l, by4, bx4,
                                                           have_top, have_left);
                    b->ref[0] = msac_decode_bool_adapt(&ts->msac,
                                            ts->cdf.m.comp_fwd_ref[1][ctx2]);
                }

                // second reference (bw)
                const int ctx3 = av1_get_bwd_ref_ctx(t->a, &t->l, by4, bx4,
                                                     have_top, have_left);
                if (msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.comp_bwd_ref[0][ctx3]))
                {
                    b->ref[1] = 6;
                } else {
                    const int ctx4 = av1_get_bwd_ref_1_ctx(t->a, &t->l, by4, bx4,
                                                           have_top, have_left);
                    b->ref[1] = 4 + msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.comp_bwd_ref[1][ctx4]);
                }
            } else {
                // unidir
                const int uctx_p = av1_get_uni_p_ctx(t->a, &t->l, by4, bx4,
                                                     have_top, have_left);
                if (msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.comp_uni_ref[0][uctx_p]))
                {
                    b->ref[0] = 4;
                    b->ref[1] = 6;
                } else {
                    const int uctx_p1 = av1_get_uni_p1_ctx(t->a, &t->l, by4, bx4,
                                                           have_top, have_left);
                    b->ref[0] = 0;
                    b->ref[1] = 1 + msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.comp_uni_ref[1][uctx_p1]);
                    if (b->ref[1] == 2) {
                        const int uctx_p2 = av1_get_uni_p2_ctx(t->a, &t->l, by4, bx4,
                                                               have_top, have_left);
                        b->ref[1] += msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.comp_uni_ref[2][uctx_p2]);
                    }
                }
            }
            if (DEBUG_BLOCK_INFO)
                printf("Post-refs[%d/%d]: r=%d\n",
                       b->ref[0], b->ref[1], ts->msac.rng);

            candidate_mv mvstack[8];
            int n_mvs, ctx;
            mv mvlist[2][2];
            av1_find_ref_mvs(mvstack, &n_mvs, mvlist, &ctx,
                             (int[2]) { b->ref[0], b->ref[1] }, f->bw, f->bh,
                             bs, bp, t->by, t->bx, ts->tiling.col_start,
                             ts->tiling.col_end, ts->tiling.row_start,
                             ts->tiling.row_end, f->libaom_cm);

            b->inter_mode = msac_decode_symbol_adapt(&ts->msac,
                                             ts->cdf.m.comp_inter_mode[ctx],
                                             N_COMP_INTER_PRED_MODES);
            if (DEBUG_BLOCK_INFO)
                printf("Post-compintermode[%d,ctx=%d,n_mvs=%d]: r=%d\n",
                       b->inter_mode, ctx, n_mvs, ts->msac.rng);

            const uint8_t *const im = av1_comp_inter_pred_modes[b->inter_mode];
            b->drl_idx = 0;
            if (b->inter_mode == NEWMV_NEWMV) {
                if (n_mvs > 1) {
                    const int drl_ctx_v1 = get_drl_context(mvstack, 0);
                    b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                             ts->cdf.m.drl_bit[drl_ctx_v1]);
                    if (b->drl_idx == 1 && n_mvs > 2) {
                        const int drl_ctx_v2 = get_drl_context(mvstack, 1);
                        b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                             ts->cdf.m.drl_bit[drl_ctx_v2]);
                    }
                    if (DEBUG_BLOCK_INFO)
                        printf("Post-drlidx[%d,n_mvs=%d]: r=%d\n",
                               b->drl_idx, n_mvs, ts->msac.rng);
                }
            } else if (im[0] == NEARMV || im[1] == NEARMV) {
                b->drl_idx = 1;
                if (n_mvs > 2) {
                    const int drl_ctx_v2 = get_drl_context(mvstack, 1);
                    b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                             ts->cdf.m.drl_bit[drl_ctx_v2]);
                    if (b->drl_idx == 2 && n_mvs > 3) {
                        const int drl_ctx_v3 = get_drl_context(mvstack, 2);
                        b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                             ts->cdf.m.drl_bit[drl_ctx_v3]);
                    }
                    if (DEBUG_BLOCK_INFO)
                        printf("Post-drlidx[%d,n_mvs=%d]: r=%d\n",
                               b->drl_idx, n_mvs, ts->msac.rng);
                }
            }

#define assign_comp_mv(idx, pfx) \
            switch (im[idx]) { \
            case NEARMV: \
            case NEARESTMV: \
                b->mv[idx] = mvstack[b->drl_idx].pfx##_mv; \
                if (!f->frame_hdr.hp) unset_hp_bit(&b->mv[idx]); \
                break; \
            case GLOBALMV: \
                has_subpel_filter |= \
                    f->frame_hdr.gmv[b->ref[idx]].type == WM_TYPE_TRANSLATION; \
                b->mv[idx] = get_gmv_2d(&f->frame_hdr.gmv[b->ref[idx]], \
                                        t->bx, t->by, bw4, bh4, &f->frame_hdr); \
                break; \
            case NEWMV: \
                b->mv[idx] = mvstack[b->drl_idx].pfx##_mv; \
                read_mv_residual(t, &b->mv[idx], &ts->cdf.mv, \
                                 !f->frame_hdr.force_integer_mv); \
                break; \
            }
            has_subpel_filter = imin(bw4, bh4) == 1 ||
                                b->inter_mode != GLOBALMV_GLOBALMV;
            assign_comp_mv(0, this);
            assign_comp_mv(1, comp);
#undef assign_comp_mv
            if (DEBUG_BLOCK_INFO)
                printf("Post-residual_mv[1:y=%d,x=%d,2:y=%d,x=%d]: r=%d\n",
                       b->mv[0].y, b->mv[0].x, b->mv[1].y, b->mv[1].x,
                       ts->msac.rng);

            // jnt_comp vs. seg vs. wedge
            int is_segwedge = 0;
            if (f->seq_hdr.masked_compound) {
                const int mask_ctx = get_mask_comp_ctx(t->a, &t->l, by4, bx4);

                is_segwedge = msac_decode_bool_adapt(&ts->msac,
                                                 ts->cdf.m.mask_comp[mask_ctx]);
                if (DEBUG_BLOCK_INFO)
                    printf("Post-segwedge_vs_jntavg[%d,ctx=%d]: r=%d\n",
                           is_segwedge, mask_ctx, ts->msac.rng);
            }

            if (!is_segwedge) {
                if (f->seq_hdr.jnt_comp) {
                    const int jnt_ctx =
                        get_jnt_comp_ctx(f->seq_hdr.order_hint_n_bits,
                                         f->cur.p.poc, f->refp[b->ref[0]].p.poc,
                                         f->refp[b->ref[1]].p.poc, t->a, &t->l,
                                         by4, bx4);
                    b->comp_type = COMP_INTER_WEIGHTED_AVG +
                        msac_decode_bool_adapt(&ts->msac,
                                               ts->cdf.m.jnt_comp[jnt_ctx]);
                    if (DEBUG_BLOCK_INFO)
                        printf("Post-jnt_comp[%d,ctx=%d[ac:%d,ar:%d,lc:%d,lr:%d]]: r=%d\n",
                               b->comp_type == COMP_INTER_AVG,
                               jnt_ctx, t->a->comp_type[bx4], t->a->ref[0][bx4],
                               t->l.comp_type[by4], t->l.ref[0][by4],
                               ts->msac.rng);
                } else {
                    b->comp_type = COMP_INTER_AVG;
                }
            } else {
                if (wedge_allowed_mask & (1 << bs)) {
                    const int ctx = av1_wedge_ctx_lut[bs];
                    b->comp_type = COMP_INTER_WEDGE -
                        msac_decode_bool_adapt(&ts->msac,
                                               ts->cdf.m.wedge_comp[ctx]);
                    if (b->comp_type == COMP_INTER_WEDGE)
                        b->wedge_idx = msac_decode_symbol_adapt(&ts->msac,
                                                ts->cdf.m.wedge_idx[ctx], 16);
                } else {
                    b->comp_type = COMP_INTER_SEG;
                }
                b->mask_sign = msac_decode_bool(&ts->msac, 128 << 7);
                if (DEBUG_BLOCK_INFO)
                    printf("Post-seg/wedge[%d,wedge_idx=%d,sign=%d]: r=%d\n",
                           b->comp_type == COMP_INTER_WEDGE,
                           b->wedge_idx, b->mask_sign, ts->msac.rng);
            }
        } else {
            b->comp_type = COMP_INTER_NONE;

            // ref
            const int ctx1 = av1_get_ref_ctx(t->a, &t->l, by4, bx4,
                                             have_top, have_left);
            if (msac_decode_bool_adapt(&ts->msac, ts->cdf.m.ref[0][ctx1])) {
                const int ctx2 = av1_get_ref_2_ctx(t->a, &t->l, by4, bx4,
                                                   have_top, have_left);
                if (msac_decode_bool_adapt(&ts->msac, ts->cdf.m.ref[1][ctx2])) {
                    b->ref[0] = 6;
                } else {
                    const int ctx3 = av1_get_ref_6_ctx(t->a, &t->l, by4, bx4,
                                                       have_top, have_left);
                    b->ref[0] = 4 + msac_decode_bool_adapt(&ts->msac,
                                                       ts->cdf.m.ref[5][ctx3]);
                }
            } else {
                const int ctx2 = av1_get_ref_3_ctx(t->a, &t->l, by4, bx4,
                                                   have_top, have_left);
                if (msac_decode_bool_adapt(&ts->msac, ts->cdf.m.ref[2][ctx2])) {
                    const int ctx3 = av1_get_ref_5_ctx(t->a, &t->l, by4, bx4,
                                                       have_top, have_left);
                    b->ref[0] = 2 + msac_decode_bool_adapt(&ts->msac,
                                                       ts->cdf.m.ref[4][ctx3]);
                } else {
                    const int ctx3 = av1_get_ref_4_ctx(t->a, &t->l, by4, bx4,
                                                       have_top, have_left);
                    b->ref[0] = msac_decode_bool_adapt(&ts->msac,
                                                       ts->cdf.m.ref[3][ctx3]);
                }
            }
            b->ref[1] = -1;
            if (DEBUG_BLOCK_INFO)
                printf("Post-ref[%d]: r=%d\n", b->ref[0], ts->msac.rng);

            candidate_mv mvstack[8];
            int n_mvs, ctx;
            mv mvlist[2][2];
            av1_find_ref_mvs(mvstack, &n_mvs, mvlist, &ctx,
                             (int[2]) { b->ref[0], -1 }, f->bw, f->bh, bs, bp,
                             t->by, t->bx, ts->tiling.col_start,
                             ts->tiling.col_end, ts->tiling.row_start,
                             ts->tiling.row_end, f->libaom_cm);

            // mode parsing and mv derivation from ref_mvs
            if (msac_decode_bool_adapt(&ts->msac, ts->cdf.m.newmv_mode[ctx & 7])) {
                if (!msac_decode_bool_adapt(&ts->msac,
                                        ts->cdf.m.globalmv_mode[(ctx >> 3) & 1]))
                {
                    b->inter_mode = GLOBALMV;
                    b->mv[0] = get_gmv_2d(&f->frame_hdr.gmv[b->ref[0]],
                                          t->bx, t->by, bw4, bh4, &f->frame_hdr);
                    has_subpel_filter = imin(bw4, bh4) == 1 ||
                        f->frame_hdr.gmv[b->ref[0]].type == WM_TYPE_TRANSLATION;
                } else {
                    has_subpel_filter = 1;
                    if (msac_decode_bool_adapt(&ts->msac,
                                       ts->cdf.m.refmv_mode[(ctx >> 4) & 15]))
                    {
                        b->inter_mode = NEARMV;
                        b->drl_idx = 1;
                        if (n_mvs > 2) {
                            const int drl_ctx_v2 = get_drl_context(mvstack, 1);
                            b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                                 ts->cdf.m.drl_bit[drl_ctx_v2]);
                            if (b->drl_idx == 2 && n_mvs > 3) {
                                const int drl_ctx_v3 =
                                    get_drl_context(mvstack, 2);
                                b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                                 ts->cdf.m.drl_bit[drl_ctx_v3]);
                            }
                        }
                    } else {
                        b->inter_mode = NEARESTMV;
                        b->drl_idx = 0;
                    }
                    if (b->drl_idx >= 2) {
                        b->mv[0] = mvstack[b->drl_idx].this_mv;
                    } else {
                        b->mv[0] = mvlist[0][b->drl_idx];
                        if (!f->frame_hdr.hp) unset_hp_bit(&b->mv[0]);
                    }
                }

                if (DEBUG_BLOCK_INFO)
                    printf("Post-intermode[%d,drl=%d,mv=y:%d,x:%d,n_mvs=%d]: r=%d\n",
                           b->inter_mode, b->drl_idx, b->mv[0].y, b->mv[0].x, n_mvs,
                           ts->msac.rng);
            } else {
                has_subpel_filter = 1;
                b->inter_mode = NEWMV;
                b->drl_idx = 0;
                if (n_mvs > 1) {
                    const int drl_ctx_v1 = get_drl_context(mvstack, 0);
                    b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                                 ts->cdf.m.drl_bit[drl_ctx_v1]);
                    if (b->drl_idx == 1 && n_mvs > 2) {
                        const int drl_ctx_v2 = get_drl_context(mvstack, 1);
                        b->drl_idx += msac_decode_bool_adapt(&ts->msac,
                                                 ts->cdf.m.drl_bit[drl_ctx_v2]);
                    }
                }
                if (n_mvs > 1) {
                    b->mv[0] = mvstack[b->drl_idx].this_mv;
                } else {
                    b->mv[0] = mvlist[0][0];
                    if (!f->frame_hdr.hp) unset_hp_bit(&b->mv[0]);
                }
                if (DEBUG_BLOCK_INFO)
                    printf("Post-intermode[%d,drl=%d]: r=%d\n",
                           b->inter_mode, b->drl_idx, ts->msac.rng);
                read_mv_residual(t, &b->mv[0], &ts->cdf.mv,
                                 !f->frame_hdr.force_integer_mv);
                if (DEBUG_BLOCK_INFO)
                    printf("Post-residualmv[mv=y:%d,x:%d]: r=%d\n",
                           b->mv[0].y, b->mv[0].x, ts->msac.rng);
            }

            // interintra flags
            const int ii_sz_grp = av1_ymode_size_context[bs];
            if (f->seq_hdr.inter_intra &&
                interintra_allowed_mask & (1 << bs) &&
                msac_decode_bool_adapt(&ts->msac, ts->cdf.m.interintra[ii_sz_grp]))
            {
                b->interintra_mode = msac_decode_symbol_adapt(&ts->msac,
                                          ts->cdf.m.interintra_mode[ii_sz_grp],
                                          N_INTER_INTRA_PRED_MODES);
                const int wedge_ctx = av1_wedge_ctx_lut[bs];
                b->interintra_type = INTER_INTRA_BLEND +
                    msac_decode_bool_adapt(&ts->msac,
                                           ts->cdf.m.interintra_wedge[wedge_ctx]);
                if (b->interintra_type == INTER_INTRA_WEDGE)
                    b->wedge_idx = msac_decode_symbol_adapt(&ts->msac,
                                            ts->cdf.m.wedge_idx[wedge_ctx], 16);
            } else {
                b->interintra_type = INTER_INTRA_NONE;
            }
            if (DEBUG_BLOCK_INFO && f->seq_hdr.inter_intra &&
                interintra_allowed_mask & (1 << bs))
            {
                printf("Post-interintra[t=%d,m=%d,w=%d]: r=%d\n",
                       b->interintra_type, b->interintra_mode,
                       b->wedge_idx, ts->msac.rng);
            }

            // motion variation
            if (f->frame_hdr.switchable_motion_mode &&
                b->interintra_type == INTER_INTRA_NONE && imin(bw4, bh4) >= 2 &&
                // is not warped global motion
                !(!f->frame_hdr.force_integer_mv && b->inter_mode == GLOBALMV &&
                  f->frame_hdr.gmv[b->ref[0]].type > WM_TYPE_TRANSLATION) &&
                // has overlappable neighbours
                ((have_left && findoddzero(&t->l.intra[by4 + 1], h4 >> 1)) ||
                 (have_top && findoddzero(&t->a->intra[bx4 + 1], w4 >> 1))))
            {
                // reaching here means the block allows obmc - check warp by
                // finding matching-ref blocks in top/left edges
                uint64_t mask[2] = { 0, 0 };
                find_matching_ref(t, intra_edge_flags, bw4, bh4, w4, h4,
                                  have_left, have_top, b->ref[0], mask);
                const int allow_warp = !f->frame_hdr.force_integer_mv &&
                    f->frame_hdr.warp_motion && (mask[0] | mask[1]);

                b->motion_mode = allow_warp ?
                    msac_decode_symbol_adapt(&ts->msac, ts->cdf.m.motion_mode[bs], 3) :
                    msac_decode_bool_adapt(&ts->msac, ts->cdf.m.obmc[bs]);
                if (b->motion_mode == MM_WARP) {
                    has_subpel_filter = 0;
                    derive_warpmv(t, bw4, bh4, mask, b->mv[0], &t->warpmv);
#define signabs(v) v < 0 ? '-' : ' ', abs(v)
                    if (DEBUG_BLOCK_INFO)
                        printf("[ %c%x %c%x %c%x\n  %c%x %c%x %c%x ]\n"
                               "alpha=%c%x, beta=%c%x, gamma=%c%x, delta=%c%x\n",
                               signabs(t->warpmv.matrix[0]),
                               signabs(t->warpmv.matrix[1]),
                               signabs(t->warpmv.matrix[2]),
                               signabs(t->warpmv.matrix[3]),
                               signabs(t->warpmv.matrix[4]),
                               signabs(t->warpmv.matrix[5]),
                               signabs(t->warpmv.alpha),
                               signabs(t->warpmv.beta),
                               signabs(t->warpmv.gamma),
                               signabs(t->warpmv.delta));
#undef signabs
                }

                if (DEBUG_BLOCK_INFO)
                    printf("Post-motionmode[%d]: r=%d [mask: 0x%" PRIu64 "x/0x%"
                           PRIu64 "x]\n", b->motion_mode, ts->msac.rng, mask[0],
                            mask[1]);
            } else {
                b->motion_mode = MM_TRANSLATION;
            }
        }

        // subpel filter
        enum FilterMode filter[2];
        if (f->frame_hdr.subpel_filter_mode == FILTER_SWITCHABLE) {
            if (has_subpel_filter) {
                const int comp = b->comp_type != COMP_INTER_NONE;
                const int ctx1 = get_filter_ctx(t->a, &t->l, comp, 0, b->ref[0],
                                                by4, bx4);
                filter[0] = msac_decode_symbol_adapt(&ts->msac,
                    ts->cdf.m.filter[0][ctx1], N_SWITCHABLE_FILTERS);
                if (f->seq_hdr.dual_filter) {
                    const int ctx2 = get_filter_ctx(t->a, &t->l, comp, 1,
                                                    b->ref[0], by4, bx4);
                    if (DEBUG_BLOCK_INFO)
                        printf("Post-subpel_filter1[%d,ctx=%d]: r=%d\n",
                               filter[0], ctx1, ts->msac.rng);
                    filter[1] = msac_decode_symbol_adapt(&ts->msac,
                        ts->cdf.m.filter[1][ctx2], N_SWITCHABLE_FILTERS);
                    if (DEBUG_BLOCK_INFO)
                        printf("Post-subpel_filter2[%d,ctx=%d]: r=%d\n",
                               filter[1], ctx2, ts->msac.rng);
                } else {
                    filter[1] = filter[0];
                    if (DEBUG_BLOCK_INFO)
                        printf("Post-subpel_filter[%d,ctx=%d]: r=%d\n",
                               filter[0], ctx1, ts->msac.rng);
                }
            } else {
                filter[0] = filter[1] = FILTER_8TAP_REGULAR;
            }
        } else {
            filter[0] = filter[1] = f->frame_hdr.subpel_filter_mode;
        }
        b->filter2d = av1_filter_2d[filter[1]][filter[0]];

        read_vartx_tree(t, b, bs, bx4, by4);

        // reconstruction
        if (f->frame_thread.pass == 1) {
            f->bd_fn.read_coef_blocks(t, bs, b);
        } else {
            f->bd_fn.recon_b_inter(t, bs, b);
        }

        const int is_globalmv =
            b->inter_mode == (is_comp ? GLOBALMV_GLOBALMV : GLOBALMV);
        const uint8_t (*const lf_lvls)[8][2] = (const uint8_t (*)[8][2])
            &ts->lflvl[b->seg_id][0][b->ref[0] + 1][!is_globalmv];
        dav1d_create_lf_mask_inter(t->lf_mask, f->lf.level, f->b4_stride,
                                   &f->frame_hdr, lf_lvls, t->bx, t->by,
                                   f->bw, f->bh, b->skip, bs, b->tx_split,
                                   b->uvtx, f->cur.p.p.layout,
                                   &t->a->tx_lpf_y[bx4], &t->l.tx_lpf_y[by4],
                                   has_chroma ? &t->a->tx_lpf_uv[cbx4] : NULL,
                                   has_chroma ? &t->l.tx_lpf_uv[cby4] : NULL);

        // context updates
        if (is_comp) {
            splat_tworef_mv(f->mvs, f->b4_stride, t->by, t->bx, bs,
                            b->inter_mode, b->ref[0], b->ref[1],
                            b->mv[0], b->mv[1]);
        } else {
            splat_oneref_mv(f->mvs, f->b4_stride, t->by, t->bx, bs,
                            b->inter_mode, b->ref[0], b->mv[0],
                            b->interintra_type);
        }
        memset(&t->l.pal_sz[by4], 0, bh4);
        memset(&t->a->pal_sz[bx4], 0, bw4);
        // see aomedia bug 2183 for why this is outside if (has_chroma)
        memset(&t->pal_sz_uv[1][by4], 0, bh4);
        memset(&t->pal_sz_uv[0][bx4], 0, bw4);
        if (has_chroma) {
            memset(&t->l.uvmode[cby4], DC_PRED, cbh4);
            memset(&t->a->uvmode[cbx4], DC_PRED, cbw4);
        }
        memset(&t->a->tx_intra[bx4], b_dim[2], bw4);
        memset(&t->l.tx_intra[by4], b_dim[3], bh4);
        memset(&t->l.comp_type[by4], b->comp_type, bh4);
        memset(&t->a->comp_type[bx4], b->comp_type, bw4);
        memset(&t->l.filter[0][by4], filter[0], bh4);
        memset(&t->a->filter[0][bx4], filter[0], bw4);
        memset(&t->l.filter[1][by4], filter[1], bh4);
        memset(&t->a->filter[1][bx4], filter[1], bw4);
        memset(&t->l.mode[by4], b->inter_mode, bh4);
        memset(&t->a->mode[bx4], b->inter_mode, bw4);
        memset(&t->l.ref[0][by4], b->ref[0], bh4);
        memset(&t->a->ref[0][bx4], b->ref[0], bw4);
        memset(&t->l.ref[1][by4], b->ref[1], bh4);
        memset(&t->a->ref[1][bx4], b->ref[1], bw4);
    }

    // update contexts
    if (f->frame_hdr.segmentation.enabled &&
        f->frame_hdr.segmentation.update_map)
    {
        uint8_t *seg_ptr = &f->cur_segmap[t->by * f->b4_stride + t->bx];
        for (int y = 0; y < bh4; y++) {
            memset(seg_ptr, b->seg_id, bw4);
            seg_ptr += f->b4_stride;
        }
    }
    memset(&t->l.seg_pred[by4], seg_pred, bh4);
    memset(&t->a->seg_pred[bx4], seg_pred, bw4);
    memset(&t->l.skip_mode[by4], b->skip_mode, bh4);
    memset(&t->a->skip_mode[bx4], b->skip_mode, bw4);
    memset(&t->l.intra[by4], b->intra, bh4);
    memset(&t->a->intra[bx4], b->intra, bw4);
    memset(&t->l.skip[by4], b->skip, bh4);
    memset(&t->a->skip[bx4], b->skip, bw4);
    if (!b->skip) {
        uint32_t *noskip_mask = &t->lf_mask->noskip_mask[by4];
        const unsigned mask = ((1ULL << bw4) - 1) << bx4;
        for (int y = 0; y < bh4; y++)
            *noskip_mask++ |= mask;
    }
}

static int decode_sb(Dav1dTileContext *const t, const enum BlockLevel bl,
                     const EdgeNode *const node)
{
    const Dav1dFrameContext *const f = t->f;
    const int hsz = 16 >> bl;
    const int have_h_split = f->bw > t->bx + hsz;
    const int have_v_split = f->bh > t->by + hsz;

    if (!have_h_split && !have_v_split) {
        assert(bl < BL_8X8);
        return decode_sb(t, bl + 1, ((const EdgeBranch *) node)->split[0]);
    }

    uint16_t *pc;
    enum BlockPartition bp;
    int ctx, bx8, by8;
    if (f->frame_thread.pass != 2) {
        if (0 && bl == BL_64X64)
            printf("poc=%d,y=%d,x=%d,bl=%d,r=%d\n",
                   f->frame_hdr.frame_offset, t->by, t->bx, bl, t->ts->msac.rng);
        bx8 = (t->bx & 31) >> 1;
        by8 = (t->by & 31) >> 1;
        ctx = get_partition_ctx(t->a, &t->l, bl, by8, bx8);
        pc = t->ts->cdf.m.partition[bl][ctx];
    }

    if (have_h_split && have_v_split) {
        if (f->frame_thread.pass == 2) {
            const Av1Block *const b = &f->frame_thread.b[t->by * f->b4_stride + t->bx];
            bp = b->bl == bl ? b->bp : PARTITION_SPLIT;
        } else {
            const unsigned n_part = bl == BL_8X8 ? N_SUB8X8_PARTITIONS :
                bl == BL_128X128 ? N_PARTITIONS - 2 : N_PARTITIONS;
            bp = msac_decode_symbol_adapt(&t->ts->msac, pc, n_part);
            if (f->cur.p.p.layout == DAV1D_PIXEL_LAYOUT_I422 &&
                (bp == PARTITION_V || bp == PARTITION_V4 ||
                 bp == PARTITION_T_LEFT_SPLIT || bp == PARTITION_T_RIGHT_SPLIT))
            {
                return 1;
            }
            if (DEBUG_BLOCK_INFO)
                printf("poc=%d,y=%d,x=%d,bl=%d,ctx=%d,bp=%d: r=%d\n",
                       f->frame_hdr.frame_offset, t->by, t->bx, bl, ctx, bp,
                       t->ts->msac.rng);
        }
        const uint8_t *const b = av1_block_sizes[bl][bp];

        switch (bp) {
        case PARTITION_NONE:
            decode_b(t, bl, b[0], PARTITION_NONE, node->o);
            break;
        case PARTITION_H:
            decode_b(t, bl, b[0], PARTITION_H, node->h[0]);
            t->by += hsz;
            decode_b(t, bl, b[0], PARTITION_H, node->h[1]);
            t->by -= hsz;
            break;
        case PARTITION_V:
            decode_b(t, bl, b[0], PARTITION_V, node->v[0]);
            t->bx += hsz;
            decode_b(t, bl, b[0], PARTITION_V, node->v[1]);
            t->bx -= hsz;
            break;
        case PARTITION_SPLIT:
            if (bl == BL_8X8) {
                const EdgeTip *const tip = (const EdgeTip *) node;
                assert(hsz == 1);
                decode_b(t, bl, BS_4x4, PARTITION_SPLIT, tip->split[0]);
                const enum Filter2d tl_filter = t->tl_4x4_filter;
                t->bx++;
                decode_b(t, bl, BS_4x4, PARTITION_SPLIT, tip->split[1]);
                t->bx--;
                t->by++;
                decode_b(t, bl, BS_4x4, PARTITION_SPLIT, tip->split[2]);
                t->bx++;
                t->tl_4x4_filter = tl_filter;
                decode_b(t, bl, BS_4x4, PARTITION_SPLIT, tip->split[3]);
                t->bx--;
                t->by--;
            } else {
                const EdgeBranch *const branch = (const EdgeBranch *) node;
                if (decode_sb(t, bl + 1, branch->split[0])) return 1;
                t->bx += hsz;
                if (decode_sb(t, bl + 1, branch->split[1])) return 1;
                t->bx -= hsz;
                t->by += hsz;
                if (decode_sb(t, bl + 1, branch->split[2])) return 1;
                t->bx += hsz;
                if (decode_sb(t, bl + 1, branch->split[3])) return 1;
                t->bx -= hsz;
                t->by -= hsz;
            }
            break;
        case PARTITION_T_TOP_SPLIT: {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            decode_b(t, bl, b[0], PARTITION_T_TOP_SPLIT, branch->tts[0]);
            t->bx += hsz;
            decode_b(t, bl, b[0], PARTITION_T_TOP_SPLIT, branch->tts[1]);
            t->bx -= hsz;
            t->by += hsz;
            decode_b(t, bl, b[1], PARTITION_T_TOP_SPLIT, branch->tts[2]);
            t->by -= hsz;
            break;
        }
        case PARTITION_T_BOTTOM_SPLIT: {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            decode_b(t, bl, b[0], PARTITION_T_BOTTOM_SPLIT, branch->tbs[0]);
            t->by += hsz;
            decode_b(t, bl, b[1], PARTITION_T_BOTTOM_SPLIT, branch->tbs[1]);
            t->bx += hsz;
            decode_b(t, bl, b[1], PARTITION_T_BOTTOM_SPLIT, branch->tbs[2]);
            t->bx -= hsz;
            t->by -= hsz;
            break;
        }
        case PARTITION_T_LEFT_SPLIT: {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            decode_b(t, bl, b[0], PARTITION_T_LEFT_SPLIT, branch->tls[0]);
            t->by += hsz;
            decode_b(t, bl, b[0], PARTITION_T_LEFT_SPLIT, branch->tls[1]);
            t->by -= hsz;
            t->bx += hsz;
            decode_b(t, bl, b[1], PARTITION_T_LEFT_SPLIT, branch->tls[2]);
            t->bx -= hsz;
            break;
        }
        case PARTITION_T_RIGHT_SPLIT: {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            decode_b(t, bl, b[0], PARTITION_T_RIGHT_SPLIT, branch->trs[0]);
            t->bx += hsz;
            decode_b(t, bl, b[1], PARTITION_T_RIGHT_SPLIT, branch->trs[1]);
            t->by += hsz;
            decode_b(t, bl, b[1], PARTITION_T_RIGHT_SPLIT, branch->trs[2]);
            t->by -= hsz;
            t->bx -= hsz;
            break;
        }
        case PARTITION_H4: {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            decode_b(t, bl, b[0], PARTITION_H4, branch->h4[0]);
            t->by += hsz >> 1;
            decode_b(t, bl, b[0], PARTITION_H4, branch->h4[1]);
            t->by += hsz >> 1;
            decode_b(t, bl, b[0], PARTITION_H4, branch->h4[2]);
            t->by += hsz >> 1;
            if (t->by < f->bh)
                decode_b(t, bl, b[0], PARTITION_H4, branch->h4[3]);
            t->by -= hsz * 3 >> 1;
            break;
        }
        case PARTITION_V4: {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            decode_b(t, bl, b[0], PARTITION_V4, branch->v4[0]);
            t->bx += hsz >> 1;
            decode_b(t, bl, b[0], PARTITION_V4, branch->v4[1]);
            t->bx += hsz >> 1;
            decode_b(t, bl, b[0], PARTITION_V4, branch->v4[2]);
            t->bx += hsz >> 1;
            if (t->bx < f->bw)
                decode_b(t, bl, b[0], PARTITION_V4, branch->v4[3]);
            t->bx -= hsz * 3 >> 1;
            break;
        }
        default: assert(0);
        }
    } else if (have_h_split) {
        unsigned is_split;
        if (f->frame_thread.pass == 2) {
            const Av1Block *const b = &f->frame_thread.b[t->by * f->b4_stride + t->bx];
            is_split = b->bl != bl;
        } else {
            const unsigned p = gather_top_partition_prob(pc, bl);
            is_split = msac_decode_bool(&t->ts->msac, p);
            if (DEBUG_BLOCK_INFO)
                printf("poc=%d,y=%d,x=%d,bl=%d,ctx=%d,bp=%d: r=%d\n",
                       f->frame_hdr.frame_offset, t->by, t->bx, bl, ctx,
                       is_split ? PARTITION_SPLIT : PARTITION_H, t->ts->msac.rng);
        }

        assert(bl < BL_8X8);
        if (is_split) {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            bp = PARTITION_SPLIT;
            if (decode_sb(t, bl + 1, branch->split[0])) return 1;
            t->bx += hsz;
            if (decode_sb(t, bl + 1, branch->split[1])) return 1;
            t->bx -= hsz;
        } else {
            bp = PARTITION_H;
            decode_b(t, bl, av1_block_sizes[bl][PARTITION_H][0], PARTITION_H,
                     node->h[0]);
        }
    } else {
        assert(have_v_split);
        unsigned is_split;
        if (f->frame_thread.pass == 2) {
            const Av1Block *const b = &f->frame_thread.b[t->by * f->b4_stride + t->bx];
            is_split = b->bl != bl;
        } else {
            const unsigned p = gather_left_partition_prob(pc, bl);
            is_split = msac_decode_bool(&t->ts->msac, p);
            if (f->cur.p.p.layout == DAV1D_PIXEL_LAYOUT_I422 && !is_split)
                return 1;
            if (DEBUG_BLOCK_INFO)
                printf("poc=%d,y=%d,x=%d,bl=%d,ctx=%d,bp=%d: r=%d\n",
                       f->frame_hdr.frame_offset, t->by, t->bx, bl, ctx,
                       is_split ? PARTITION_SPLIT : PARTITION_V, t->ts->msac.rng);
        }

        assert(bl < BL_8X8);
        if (is_split) {
            const EdgeBranch *const branch = (const EdgeBranch *) node;
            bp = PARTITION_SPLIT;
            if (decode_sb(t, bl + 1, branch->split[0])) return 1;
            t->by += hsz;
            if (decode_sb(t, bl + 1, branch->split[2])) return 1;
            t->by -= hsz;
        } else {
            bp = PARTITION_V;
            decode_b(t, bl, av1_block_sizes[bl][PARTITION_V][0], PARTITION_V,
                     node->v[0]);
        }
    }

    if (f->frame_thread.pass != 2 && (bp != PARTITION_SPLIT || bl == BL_8X8)) {
        memset(&t->a->partition[bx8], av1_al_part_ctx[0][bl][bp], hsz);
        memset(&t->l.partition[by8], av1_al_part_ctx[1][bl][bp], hsz);
    }

    return 0;
}

static void reset_context(BlockContext *const ctx, const int keyframe, const int pass) {
    memset(ctx->intra, keyframe, sizeof(ctx->intra));
    memset(ctx->uvmode, DC_PRED, sizeof(ctx->uvmode));
    if (keyframe)
        memset(ctx->mode, DC_PRED, sizeof(ctx->mode));

    if (pass == 2) return;

    memset(ctx->partition, 0, sizeof(ctx->partition));
    memset(ctx->skip, 0, sizeof(ctx->skip));
    memset(ctx->skip_mode, 0, sizeof(ctx->skip_mode));
    memset(ctx->tx_lpf_y, 2, sizeof(ctx->tx_lpf_y));
    memset(ctx->tx_lpf_uv, 1, sizeof(ctx->tx_lpf_uv));
    memset(ctx->tx_intra, -1, sizeof(ctx->tx_intra));
    memset(ctx->tx, TX_64X64, sizeof(ctx->tx));
    if (!keyframe) {
        memset(ctx->ref, -1, sizeof(ctx->ref));
        memset(ctx->comp_type, 0, sizeof(ctx->comp_type));
        memset(ctx->mode, NEARESTMV, sizeof(ctx->mode));
    }
    memset(ctx->lcoef, 0x40, sizeof(ctx->lcoef));
    memset(ctx->ccoef, 0x40, sizeof(ctx->ccoef));
    memset(ctx->filter, N_SWITCHABLE_FILTERS, sizeof(ctx->filter));
    memset(ctx->seg_pred, 0, sizeof(ctx->seg_pred));
    memset(ctx->pal_sz, 0, sizeof(ctx->pal_sz));
}

static void setup_tile(Dav1dTileState *const ts,
                       const Dav1dFrameContext *const f,
                       const uint8_t *const data, const size_t sz,
                       const int tile_row, const int tile_col,
                       const int tile_start_off)
{
    const int col_sb_start = f->frame_hdr.tiling.col_start_sb[tile_col];
    const int col_sb128_start = col_sb_start >> !f->seq_hdr.sb128;
    const int col_sb_end = f->frame_hdr.tiling.col_start_sb[tile_col + 1];
    const int row_sb_start = f->frame_hdr.tiling.row_start_sb[tile_row];
    const int row_sb_end = f->frame_hdr.tiling.row_start_sb[tile_row + 1];
    const int sb_shift = f->sb_shift;

    ts->frame_thread.pal_idx = &f->frame_thread.pal_idx[tile_start_off * 2];
    ts->frame_thread.cf = &((int32_t *) f->frame_thread.cf)[tile_start_off * 3];
    ts->cdf = *f->in_cdf.cdf;
    ts->last_qidx = f->frame_hdr.quant.yac;
    memset(ts->last_delta_lf, 0, sizeof(ts->last_delta_lf));

    msac_init(&ts->msac, data, sz);

    ts->tiling.row = tile_row;
    ts->tiling.col = tile_col;
    ts->tiling.col_start = col_sb_start << sb_shift;
    ts->tiling.col_end = imin(col_sb_end << sb_shift, f->bw);
    ts->tiling.row_start = row_sb_start << sb_shift;
    ts->tiling.row_end = imin(row_sb_end << sb_shift, f->bh);

    // Reference Restoration Unit (used for exp coding)
    Av1Filter *const lf_mask =
        f->lf.mask + (ts->tiling.row_start >> 5) * f->sb128w + col_sb128_start;
    const int unit_idx = ((ts->tiling.row_start & 16) >> 3) +
                         ((ts->tiling.col_start & 16) >> 4);
    for (int p = 0; p < 3; p++) {
        ts->lr_ref[p] = &lf_mask->lr[p][unit_idx];
        ts->lr_ref[p]->filter_v[0] = 3;
        ts->lr_ref[p]->filter_v[1] = -7;
        ts->lr_ref[p]->filter_v[2] = 15;
        ts->lr_ref[p]->filter_h[0] = 3;
        ts->lr_ref[p]->filter_h[1] = -7;
        ts->lr_ref[p]->filter_h[2] = 15;
        ts->lr_ref[p]->sgr_weights[0] = -32;
        ts->lr_ref[p]->sgr_weights[1] = 31;
    }

    if (f->n_tc > 1)
        atomic_init(&ts->progress, row_sb_start);
}

int decode_tile_sbrow(Dav1dTileContext *const t) {
    const Dav1dFrameContext *const f = t->f;
    const enum BlockLevel root_bl = f->seq_hdr.sb128 ? BL_128X128 : BL_64X64;
    Dav1dTileState *const ts = t->ts;
    const Dav1dContext *const c = f->c;
    const int sb_step = f->sb_step;
    const int tile_row = ts->tiling.row, tile_col = ts->tiling.col;
    const int col_sb_start = f->frame_hdr.tiling.col_start_sb[tile_col];
    const int col_sb128_start = col_sb_start >> !f->seq_hdr.sb128;

    reset_context(&t->l, !(f->frame_hdr.frame_type & 1), f->frame_thread.pass);
    if (f->frame_thread.pass == 2) {
        for (t->bx = ts->tiling.col_start,
             t->a = f->a + col_sb128_start + tile_row * f->sb128w;
             t->bx < ts->tiling.col_end; t->bx += sb_step)
        {
            if (decode_sb(t, root_bl, c->intra_edge.root[root_bl]))
                return 1;
            if (t->bx & 16 || f->seq_hdr.sb128)
                t->a++;
        }
        f->bd_fn.backup_ipred_edge(t);
        return 0;
    }

    const int ss_ver = f->cur.p.p.layout == DAV1D_PIXEL_LAYOUT_I420;
    const int ss_hor = f->cur.p.p.layout != DAV1D_PIXEL_LAYOUT_I444;

    if (c->n_fc > 1 && f->frame_hdr.use_ref_frame_mvs) {
        for (int n = 0; n < 7; n++)
            dav1d_thread_picture_wait(&f->refp[n], 4 * (t->by + sb_step),
                                      PLANE_TYPE_BLOCK);
        av1_init_ref_mv_tile_row(f->libaom_cm,
                                 ts->tiling.col_start, ts->tiling.col_end,
                                 t->by, imin(t->by + sb_step, f->bh));
    }
    memset(t->pal_sz_uv[1], 0, sizeof(*t->pal_sz_uv));
    const int sb128y = t->by >> 5;
    for (t->bx = ts->tiling.col_start, t->a = f->a + col_sb128_start + tile_row * f->sb128w,
         t->lf_mask = f->lf.mask + sb128y * f->sb128w + col_sb128_start;
         t->bx < ts->tiling.col_end; t->bx += sb_step)
    {
        if (root_bl == BL_128X128) {
            t->cur_sb_cdef_idx_ptr = t->lf_mask->cdef_idx;
            t->cur_sb_cdef_idx_ptr[0] = -1;
            t->cur_sb_cdef_idx_ptr[1] = -1;
            t->cur_sb_cdef_idx_ptr[2] = -1;
            t->cur_sb_cdef_idx_ptr[3] = -1;
        } else {
            t->cur_sb_cdef_idx_ptr =
                &t->lf_mask->cdef_idx[((t->bx & 16) >> 4) +
                                      ((t->by & 16) >> 3)];
            t->cur_sb_cdef_idx_ptr[0] = -1;
        }
        // Restoration filter
        for (int p = 0; p < 3; p++) {
            if (f->frame_hdr.restoration.type[p] == RESTORATION_NONE)
                continue;
            const int by = t->by >> (ss_ver & !!p);
            const int bx = t->bx >> (ss_hor & !!p);
            const int bh = f->bh >> (ss_ver & !!p);
            const int bw = f->bw >> (ss_hor & !!p);

            const int unit_size_log2 =
                f->frame_hdr.restoration.unit_size[!!p];
            // 4pel unit size
            const int b_unit_size = 1 << (unit_size_log2 - 2);
            const unsigned mask = b_unit_size - 1;
            if (by & mask || bx & mask) continue;
            const int half_unit = b_unit_size >> 1;
            // Round half up at frame boundaries, if there's more than one
            // restoration unit
            const int bottom_round = by && by + half_unit > bh;
            const int right_round = bx && bx + half_unit > bw;
            if (bottom_round || right_round) continue;
            const int unit_idx = ((t->by & 16) >> 3) + ((t->bx & 16) >> 4);
            Av1RestorationUnit *const lr = &t->lf_mask->lr[p][unit_idx];
            const enum RestorationType frame_type =
                f->frame_hdr.restoration.type[p];

            if (frame_type == RESTORATION_SWITCHABLE) {
                const int filter =
                    msac_decode_symbol_adapt(&ts->msac,
                                             ts->cdf.m.restore_switchable, 3);
                lr->type = filter ? filter == 2 ? RESTORATION_SGRPROJ :
                                                  RESTORATION_WIENER :
                                    RESTORATION_NONE;
            } else {
                const unsigned type =
                    msac_decode_bool_adapt(&ts->msac,
                                           frame_type == RESTORATION_WIENER ?
                                               ts->cdf.m.restore_wiener :
                                               ts->cdf.m.restore_sgrproj);
                lr->type = type ? frame_type : RESTORATION_NONE;
            }

            if (lr->type == RESTORATION_WIENER) {
                lr->filter_v[0] =
                    !p ? msac_decode_subexp(&ts->msac,
                                            ts->lr_ref[p]->filter_v[0] + 5, 16,
                                            1) - 5:
                         0;
                lr->filter_v[1] =
                    msac_decode_subexp(&ts->msac,
                                       ts->lr_ref[p]->filter_v[1] + 23, 32,
                                       2) - 23;
                lr->filter_v[2] =
                    msac_decode_subexp(&ts->msac,
                                       ts->lr_ref[p]->filter_v[2] + 17, 64,
                                       3) - 17;

                lr->filter_h[0] =
                    !p ? msac_decode_subexp(&ts->msac,
                                            ts->lr_ref[p]->filter_h[0] + 5, 16,
                                            1) - 5:
                        0;
                lr->filter_h[1] =
                    msac_decode_subexp(&ts->msac,
                                       ts->lr_ref[p]->filter_h[1] + 23, 32,
                                       2) - 23;
                lr->filter_h[2] =
                    msac_decode_subexp(&ts->msac,
                                       ts->lr_ref[p]->filter_h[2] + 17, 64,
                                       3) - 17;
                memcpy(lr->sgr_weights, ts->lr_ref[p]->sgr_weights, sizeof(lr->sgr_weights));
                ts->lr_ref[p] = lr;
                if (DEBUG_BLOCK_INFO)
                    printf("Post-lr_wiener[pl=%d,v[%d,%d,%d],h[%d,%d,%d]]: r=%d\n",
                           p, lr->filter_v[0], lr->filter_v[1],
                           lr->filter_v[2], lr->filter_h[0],
                           lr->filter_h[1], lr->filter_h[2], ts->msac.rng);
            } else if (lr->type == RESTORATION_SGRPROJ) {
                const unsigned idx = msac_decode_bools(&ts->msac, 4);
                lr->sgr_idx = idx;
                lr->sgr_weights[0] = sgr_params[idx][0] ?
                    msac_decode_subexp(&ts->msac,
                                       ts->lr_ref[p]->sgr_weights[0] + 96, 128,
                                       4) - 96 :
                    0;
                lr->sgr_weights[1] = sgr_params[idx][1] ?
                    msac_decode_subexp(&ts->msac,
                                       ts->lr_ref[p]->sgr_weights[1] + 32, 128,
                                       4) - 32 :
                    iclip(128 - lr->sgr_weights[0], -32, 95);
                memcpy(lr->filter_v, ts->lr_ref[p]->filter_v, sizeof(lr->filter_v));
                memcpy(lr->filter_h, ts->lr_ref[p]->filter_h, sizeof(lr->filter_h));
                ts->lr_ref[p] = lr;
                if (DEBUG_BLOCK_INFO)
                    printf("Post-lr_sgrproj[pl=%d,idx=%d,w[%d,%d]]: r=%d\n",
                           p, lr->sgr_idx, lr->sgr_weights[0],
                           lr->sgr_weights[1], ts->msac.rng);
            }
        }
        if (decode_sb(t, root_bl, c->intra_edge.root[root_bl]))
            return 1;
        if (t->bx & 16 || f->seq_hdr.sb128) {
            t->a++;
            t->lf_mask++;
        }
    }

    // backup pre-loopfilter pixels for intra prediction of the next sbrow
    if (f->frame_thread.pass != 1)
        f->bd_fn.backup_ipred_edge(t);

    // backup t->a/l.tx_lpf_y/uv at tile boundaries to use them to "fix"
    // up the initial value in neighbour tiles when running the loopfilter
    int align_h = (f->bh + 31) & ~31;
    memcpy(&f->lf.tx_lpf_right_edge[0][align_h * tile_col + t->by],
           &t->l.tx_lpf_y[t->by & 16], sb_step);
    align_h >>= 1;
    memcpy(&f->lf.tx_lpf_right_edge[1][align_h * tile_col + (t->by >> 1)],
           &t->l.tx_lpf_uv[(t->by & 16) >> 1], sb_step >> 1);

    return 0;
}

int decode_frame(Dav1dFrameContext *const f) {
    const Dav1dContext *const c = f->c;

    if (f->n_tc > 1) {
        if (f->frame_hdr.tiling.cols * f->sbh > f->tile_thread.titsati_sz) {
            f->tile_thread.task_idx_to_sby_and_tile_idx =
                malloc(sizeof(*f->tile_thread.task_idx_to_sby_and_tile_idx) *
                       f->frame_hdr.tiling.cols * f->sbh);
            if (!f->tile_thread.task_idx_to_sby_and_tile_idx) return -ENOMEM;
            f->tile_thread.titsati_sz = f->frame_hdr.tiling.cols * f->sbh;
        }
        if (f->tile_thread.titsati_init[0] != f->frame_hdr.tiling.cols ||
            f->tile_thread.titsati_init[1] != f->sbh)
        {
            for (int tile_row = 0, tile_idx = 0;
                 tile_row < f->frame_hdr.tiling.rows; tile_row++)
            {
                for (int sby = f->frame_hdr.tiling.row_start_sb[tile_row];
                     sby < f->frame_hdr.tiling.row_start_sb[tile_row + 1]; sby++)
                {
                    for (int tile_col = 0; tile_col < f->frame_hdr.tiling.cols;
                         tile_col++, tile_idx++)
                    {
                        f->tile_thread.task_idx_to_sby_and_tile_idx[tile_idx][0] = sby;
                        f->tile_thread.task_idx_to_sby_and_tile_idx[tile_idx][1] =
                            tile_row * f->frame_hdr.tiling.cols + tile_col;
                    }
                }
            }
            f->tile_thread.titsati_init[0] = f->frame_hdr.tiling.cols;
            f->tile_thread.titsati_init[1] = f->sbh;
        }
    }

    if (f->frame_hdr.tiling.cols * f->frame_hdr.tiling.rows > f->n_ts) {
        f->ts = realloc(f->ts, f->frame_hdr.tiling.cols *
                               f->frame_hdr.tiling.rows * sizeof(*f->ts));
        if (!f->ts) return -ENOMEM;
        for (int n = f->n_ts;
             n < f->frame_hdr.tiling.cols * f->frame_hdr.tiling.rows; n++)
        {
            Dav1dTileState *const ts = &f->ts[n];
            pthread_mutex_init(&ts->tile_thread.lock, NULL);
            pthread_cond_init(&ts->tile_thread.cond, NULL);
        }
        if (c->n_fc > 1) {
            freep(&f->frame_thread.tile_start_off);
            f->frame_thread.tile_start_off =
                malloc(sizeof(*f->frame_thread.tile_start_off) *
                       f->frame_hdr.tiling.cols * f->frame_hdr.tiling.rows);
        }
        f->n_ts = f->frame_hdr.tiling.cols * f->frame_hdr.tiling.rows;
    }

    if (c->n_fc > 1) {
        int tile_idx = 0;
        for (int tile_row = 0; tile_row < f->frame_hdr.tiling.rows; tile_row++) {
            int row_off = f->frame_hdr.tiling.row_start_sb[tile_row] *
                          f->sb_step * 4 * f->sb128w * 128;
            int b_diff = (f->frame_hdr.tiling.row_start_sb[tile_row + 1] -
                          f->frame_hdr.tiling.row_start_sb[tile_row]) * f->sb_step * 4;
            for (int tile_col = 0; tile_col < f->frame_hdr.tiling.cols; tile_col++) {
                f->frame_thread.tile_start_off[tile_idx++] = row_off + b_diff *
                    f->frame_hdr.tiling.col_start_sb[tile_col] * f->sb_step * 4;
            }
        }
    }

    if (f->sb128w * f->frame_hdr.tiling.rows > f->a_sz) {
        freep(&f->a);
        f->a = malloc(f->sb128w * f->frame_hdr.tiling.rows * sizeof(*f->a));
        if (!f->a) return -ENOMEM;
        f->a_sz = f->sb128w * f->frame_hdr.tiling.rows;
    }

    // update allocation of block contexts for above
    if (f->sb128w > f->lf.line_sz) {
        dav1d_freep_aligned(&f->lf.cdef_line);
        dav1d_freep_aligned(&f->lf.lr_lpf_line);

        // note that we allocate all pixel arrays as if we were dealing with
        // 10 bits/component data
        uint16_t *ptr = f->lf.cdef_line =
            dav1d_alloc_aligned(f->b4_stride * 4 * 12 * sizeof(uint16_t), 32);

        uint16_t *lr_ptr = f->lf.lr_lpf_line =
            dav1d_alloc_aligned(f->b4_stride * 4 * 3 * 12 * sizeof(uint16_t), 32);

        for (int pl = 0; pl <= 2; pl++) {
            f->lf.cdef_line_ptr[0][pl][0] = ptr + f->b4_stride * 4 * 0;
            f->lf.cdef_line_ptr[0][pl][1] = ptr + f->b4_stride * 4 * 1;
            f->lf.cdef_line_ptr[1][pl][0] = ptr + f->b4_stride * 4 * 2;
            f->lf.cdef_line_ptr[1][pl][1] = ptr + f->b4_stride * 4 * 3;
            ptr += f->b4_stride * 4 * 4;

            f->lf.lr_lpf_line_ptr[pl] = lr_ptr;
            lr_ptr += f->b4_stride * 4 * 12;
        }

        f->lf.line_sz = f->sb128w;
    }

    // update allocation for loopfilter masks
    if (f->sb128w * f->sb128h > f->lf.mask_sz) {
        freep(&f->lf.mask);
        freep(&f->lf.level);
        freep(&f->frame_thread.b);
        f->lf.mask = malloc(f->sb128w * f->sb128h * sizeof(*f->lf.mask));
        f->lf.level = malloc(f->sb128w * f->sb128h * 32 * 32 *
                             sizeof(*f->lf.level));
        if (!f->lf.mask || !f->lf.level) return -ENOMEM;
        if (c->n_fc > 1) {
            freep(&f->frame_thread.b);
            freep(&f->frame_thread.cbi);
            dav1d_freep_aligned(&f->frame_thread.cf);
            dav1d_freep_aligned(&f->frame_thread.pal_idx);
            freep(&f->frame_thread.pal);
            f->frame_thread.b = malloc(sizeof(*f->frame_thread.b) *
                                       f->sb128w * f->sb128h * 32 * 32);
            f->frame_thread.pal = malloc(sizeof(*f->frame_thread.pal) *
                                         f->sb128w * f->sb128h * 16 * 16);
            f->frame_thread.pal_idx =
                dav1d_alloc_aligned(sizeof(*f->frame_thread.pal_idx) *
                                    f->sb128w * f->sb128h * 128 * 128 * 2, 32);
            f->frame_thread.cbi = malloc(sizeof(*f->frame_thread.cbi) *
                                         f->sb128w * f->sb128h * 32 * 32);
            f->frame_thread.cf =
                dav1d_alloc_aligned(sizeof(int32_t) * 3 *
                                    f->sb128w * f->sb128h * 128 * 128, 32);
            if (!f->frame_thread.b || !f->frame_thread.pal_idx ||
                !f->frame_thread.cf)
            {
                return -ENOMEM;
            }
            memset(f->frame_thread.cf, 0,
                   sizeof(int32_t) * 3 * f->sb128w * f->sb128h * 128 * 128);
        }
        f->lf.mask_sz = f->sb128w * f->sb128h;
    }
    if (f->frame_hdr.loopfilter.sharpness != f->lf.last_sharpness) {
        dav1d_calc_eih(&f->lf.lim_lut, f->frame_hdr.loopfilter.sharpness);
        f->lf.last_sharpness = f->frame_hdr.loopfilter.sharpness;
    }
    dav1d_calc_lf_values(f->lf.lvl, &f->frame_hdr, (int8_t[4]) { 0, 0, 0, 0 });
    memset(f->lf.mask, 0, sizeof(*f->lf.mask) * f->sb128w * f->sb128h);

    if (f->sbh * f->sb128w * 128 > f->ipred_edge_sz) {
        dav1d_freep_aligned(&f->ipred_edge[0]);
        uint16_t *ptr = f->ipred_edge[0] =
            dav1d_alloc_aligned(f->sb128w * 128 * f->sbh * 3 * sizeof(uint16_t), 32);
        if (!f->ipred_edge[0]) return -ENOMEM;
        f->ipred_edge_sz = f->sbh * f->sb128w * 128;
        f->ipred_edge[1] = &ptr[f->ipred_edge_sz];
        f->ipred_edge[2] = &ptr[f->ipred_edge_sz * 2];
    }

    if (f->sb128h > f->lf.re_sz) {
        freep(&f->lf.tx_lpf_right_edge[0]);
        f->lf.tx_lpf_right_edge[0] = malloc((f->sb128h * 32 * 2) *
                                            f->frame_hdr.tiling.cols);
        if (!f->lf.tx_lpf_right_edge[0]) return -ENOMEM;
        f->lf.tx_lpf_right_edge[1] = f->lf.tx_lpf_right_edge[0] +
                                     f->sb128h * 32 * f->frame_hdr.tiling.cols;
        f->lf.re_sz = f->sb128h;
    }

    // init ref mvs
    if ((f->frame_hdr.frame_type & 1) || f->frame_hdr.allow_intrabc) {
        f->mvs = f->mvs_ref->data;
        const int order_hint_n_bits = f->seq_hdr.order_hint * f->seq_hdr.order_hint_n_bits;
        av1_init_ref_mv_common(f->libaom_cm, f->bw >> 1, f->bh >> 1,
                               f->b4_stride, f->seq_hdr.sb128,
                               f->mvs, f->ref_mvs, f->cur.p.poc, f->refpoc,
                               f->refrefpoc, f->frame_hdr.gmv,
                               f->frame_hdr.hp, f->frame_hdr.force_integer_mv,
                               f->frame_hdr.use_ref_frame_mvs,
                               order_hint_n_bits);
        if (c->n_fc == 1 && f->frame_hdr.use_ref_frame_mvs)
            av1_init_ref_mv_tile_row(f->libaom_cm, 0, f->bw, 0, f->bh);
    }

    // setup dequant tables
    init_quant_tables(&f->seq_hdr, &f->frame_hdr, f->frame_hdr.quant.yac, f->dq);
    if (f->frame_hdr.quant.qm)
        for (int j = 0; j < N_RECT_TX_SIZES; j++) {
            f->qm[0][j][0] = av1_qm_tbl[f->frame_hdr.quant.qm_y][0][j];
            f->qm[0][j][1] = av1_qm_tbl[f->frame_hdr.quant.qm_u][1][j];
            f->qm[0][j][2] = av1_qm_tbl[f->frame_hdr.quant.qm_v][1][j];
        }
    for (int i = f->frame_hdr.quant.qm; i < 2; i++)
        for (int tx = 0; tx < N_RECT_TX_SIZES; tx++)
            for (int pl = 0; pl < 3; pl++)
                f->qm[i][tx][pl] = av1_qm_tbl[15][!!pl][tx];

    // setup jnt_comp weights
    if (f->frame_hdr.switchable_comp_refs) {
        for (int i = 0; i < 7; i++) {
            const unsigned ref0poc = f->refp[i].p.poc;

            for (int j = i + 1; j < 7; j++) {
                const unsigned ref1poc = f->refp[j].p.poc;

                const unsigned d1 = imin(abs(get_poc_diff(f->seq_hdr.order_hint_n_bits,
                                                          ref0poc, f->cur.p.poc)), 31);
                const unsigned d0 = imin(abs(get_poc_diff(f->seq_hdr.order_hint_n_bits,
                                                          ref1poc, f->cur.p.poc)), 31);
                const int order = d0 <= d1;

                static const uint8_t quant_dist_weight[3][2] = {
                    { 2, 3 }, { 2, 5 }, { 2, 7 }
                };
                static const uint8_t quant_dist_lookup_table[4][2] = {
                    { 9, 7 }, { 11, 5 }, { 12, 4 }, { 13, 3 }
                };

                int k;
                for (k = 0; k < 3; k++) {
                    const int c0 = quant_dist_weight[k][order];
                    const int c1 = quant_dist_weight[k][!order];
                    const int d0_c0 = d0 * c0;
                    const int d1_c1 = d1 * c1;
                    if ((d0 > d1 && d0_c0 < d1_c1) || (d0 <= d1 && d0_c0 > d1_c1)) break;
                }

                f->jnt_weights[i][j] = quant_dist_lookup_table[k][order];
            }
        }
    }

    // init loopfilter pointers
    f->lf.mask_ptr = f->lf.mask;
    f->lf.p[0] = f->cur.p.data[0];
    f->lf.p[1] = f->cur.p.data[1];
    f->lf.p[2] = f->cur.p.data[2];
    f->lf.tile_row = 1;

    cdf_thread_wait(&f->in_cdf);

    // parse individual tiles per tile group
    int update_set = 0, tile_idx = 0;
    const unsigned tile_col_mask = (1 << f->frame_hdr.tiling.log2_cols) - 1;
    for (int i = 0; i < f->n_tile_data; i++) {
        const uint8_t *data = f->tile[i].data.data;
        size_t size = f->tile[i].data.sz;

        const int last_tile_row_plus1 = 1 + (f->tile[i].end >> f->frame_hdr.tiling.log2_cols);
        const int last_tile_col_plus1 = 1 + (f->tile[i].end & tile_col_mask);
        const int empty_tile_cols = imax(0, last_tile_col_plus1 - f->frame_hdr.tiling.cols);
        const int empty_tile_rows = imax(0, last_tile_row_plus1 - f->frame_hdr.tiling.rows);
        const int empty_tiles =
            (empty_tile_rows << f->frame_hdr.tiling.log2_cols) + empty_tile_cols;
        for (int j = f->tile[i].start; j <= f->tile[i].end - empty_tiles; j++) {
            const int tile_row = j >> f->frame_hdr.tiling.log2_cols;
            const int tile_col = j & tile_col_mask;

            if (tile_col >= f->frame_hdr.tiling.cols) continue;
            if (tile_row >= f->frame_hdr.tiling.rows) continue;

            size_t tile_sz;
            if (j == f->tile[i].end - empty_tiles) {
                tile_sz = size;
            } else {
                tile_sz = 0;
                for (int k = 0; k < f->frame_hdr.tiling.n_bytes; k++)
                    tile_sz |= *data++ << (k * 8);
                tile_sz++;
                size -= f->frame_hdr.tiling.n_bytes;
                if (tile_sz > size) goto error;
            }

            setup_tile(&f->ts[tile_row * f->frame_hdr.tiling.cols + tile_col],
                       f, data, tile_sz, tile_row, tile_col,
                       c->n_fc > 1 ? f->frame_thread.tile_start_off[tile_idx++] : 0);
            if (j == f->frame_hdr.tiling.update && f->frame_hdr.refresh_context)
                update_set = 1;
            data += tile_sz;
            size -= tile_sz;
        }
    }

    cdf_thread_unref(&f->in_cdf);

    // 2-pass decoding:
    // - enabled for frame-threading, so that one frame can do symbol parsing
    //   as another (or multiple) are doing reconstruction. One advantage here
    //   is that although reconstruction is limited by reference availability,
    //   symbol parsing is not. Therefore, symbol parsing can effectively use
    //   row and col tile threading, but reconstruction only col tile threading;
    // - pass 0 means no 2-pass;
    // - pass 1 means symbol parsing only;
    // - pass 2 means reconstruction and loop filtering.

    const int uses_2pass = c->n_fc > 1 && f->frame_hdr.refresh_context;
    for (f->frame_thread.pass = uses_2pass;
         f->frame_thread.pass <= 2 * uses_2pass; f->frame_thread.pass++)
    {
        const enum PlaneType progress_plane_type =
            f->frame_thread.pass == 0 ? PLANE_TYPE_ALL :
            f->frame_thread.pass == 1 ? PLANE_TYPE_BLOCK : PLANE_TYPE_Y;

        for (int n = 0; n < f->sb128w * f->frame_hdr.tiling.rows; n++)
            reset_context(&f->a[n], !(f->frame_hdr.frame_type & 1), f->frame_thread.pass);

        if (f->n_tc == 1) {
            Dav1dTileContext *const t = f->tc;

            // no tile threading - we explicitly interleave tile/sbrow decoding
            // and post-filtering, so that the full process runs in-line, so
            // that frame threading is still possible
            for (int tile_row = 0; tile_row < f->frame_hdr.tiling.rows; tile_row++) {
                for (int sby = f->frame_hdr.tiling.row_start_sb[tile_row];
                     sby < f->frame_hdr.tiling.row_start_sb[tile_row + 1]; sby++)
                {
                    t->by = sby << (4 + f->seq_hdr.sb128);
                    for (int tile_col = 0; tile_col < f->frame_hdr.tiling.cols; tile_col++) {
                        t->ts = &f->ts[tile_row * f->frame_hdr.tiling.cols + tile_col];

                        int res;
                        if ((res = decode_tile_sbrow(t)))
                            return res;
                    }

                    // loopfilter + cdef + restoration
                    if (f->frame_thread.pass != 1)
                        f->bd_fn.filter_sbrow(f, sby);
                    dav1d_thread_picture_signal(&f->cur, (sby + 1) * f->sb_step * 4,
                                                progress_plane_type);
                }
            }
        } else {
            // signal available tasks to worker threads
            int num_tasks;

            const uint64_t all_mask = ~0ULL >> (64 - f->n_tc);
            pthread_mutex_lock(&f->tile_thread.lock);
            while (f->tile_thread.available != all_mask)
                pthread_cond_wait(&f->tile_thread.icond, &f->tile_thread.lock);
            assert(!f->tile_thread.tasks_left);
            if (f->frame_thread.pass == 1 || f->n_tc >= f->frame_hdr.tiling.cols) {
                // we can (or in fact, if >, we need to) do full tile decoding.
                // loopfilter happens below
                num_tasks = f->frame_hdr.tiling.cols * f->frame_hdr.tiling.rows;
            } else {
                // we need to interleave sbrow decoding for all tile cols in a
                // tile row, since otherwise subsequent threads will be blocked
                // waiting for the post-filter to complete
                num_tasks = f->sbh * f->frame_hdr.tiling.cols;
            }
            f->tile_thread.num_tasks = f->tile_thread.tasks_left = num_tasks;
            pthread_cond_broadcast(&f->tile_thread.cond);
            pthread_mutex_unlock(&f->tile_thread.lock);

            // loopfilter + cdef + restoration
            for (int tile_row = 0; tile_row < f->frame_hdr.tiling.rows; tile_row++) {
                for (int sby = f->frame_hdr.tiling.row_start_sb[tile_row];
                     sby < f->frame_hdr.tiling.row_start_sb[tile_row + 1]; sby++)
                {
                    for (int tile_col = 0; tile_col < f->frame_hdr.tiling.cols;
                         tile_col++)
                    {
                        Dav1dTileState *const ts =
                            &f->ts[tile_row * f->frame_hdr.tiling.cols + tile_col];

                        if (atomic_load(&ts->progress) <= sby) {
                            pthread_mutex_lock(&ts->tile_thread.lock);
                            while (atomic_load(&ts->progress) <= sby)
                                pthread_cond_wait(&ts->tile_thread.cond,
                                                  &ts->tile_thread.lock);
                            pthread_mutex_unlock(&ts->tile_thread.lock);
                        }
                    }

                    // loopfilter + cdef + restoration
                    if (f->frame_thread.pass != 1)
                        f->bd_fn.filter_sbrow(f, sby);
                    dav1d_thread_picture_signal(&f->cur, (sby + 1) * f->sb_step * 4,
                                                progress_plane_type);
                }
            }
        }

        if (f->frame_thread.pass <= 1 && f->frame_hdr.refresh_context) {
            // cdf update
            if (update_set)
                av1_update_tile_cdf(&f->frame_hdr, f->out_cdf.cdf,
                                    &f->ts[f->frame_hdr.tiling.update].cdf);
            cdf_thread_signal(&f->out_cdf);
            cdf_thread_unref(&f->out_cdf);
        }
        if (f->frame_thread.pass == 1) {
            assert(c->n_fc > 1);
            for (int tile_idx = 0;
                 tile_idx < f->frame_hdr.tiling.rows * f->frame_hdr.tiling.cols;
                 tile_idx++)
            {
                Dav1dTileState *const ts = &f->ts[tile_idx];
                const int tile_start_off = f->frame_thread.tile_start_off[tile_idx];
                ts->frame_thread.pal_idx = &f->frame_thread.pal_idx[tile_start_off * 2];
                ts->frame_thread.cf = &((int32_t *) f->frame_thread.cf)[tile_start_off * 3];
                if (f->n_tc > 0)
                    atomic_init(&ts->progress, 0);
            }
        }
    }

    dav1d_thread_picture_signal(&f->cur, UINT_MAX, PLANE_TYPE_ALL);

    for (int i = 0; i < 7; i++) {
        if (f->refp[i].p.data[0])
            dav1d_thread_picture_unref(&f->refp[i]);
        if (f->ref_mvs_ref[i])
            dav1d_ref_dec(f->ref_mvs_ref[i]);
    }

    dav1d_thread_picture_unref(&f->cur);
    if (f->cur_segmap_ref)
        dav1d_ref_dec(f->cur_segmap_ref);
    if (f->prev_segmap_ref)
        dav1d_ref_dec(f->prev_segmap_ref);
    if (f->mvs_ref)
        dav1d_ref_dec(f->mvs_ref);

    for (int i = 0; i < f->n_tile_data; i++)
        dav1d_data_unref(&f->tile[i].data);

    return 0;

error:
    for (int i = 0; i < f->n_tile_data; i++)
        dav1d_data_unref(&f->tile[i].data);

    return -EINVAL;
}

int submit_frame(Dav1dContext *const c) {
    Dav1dFrameContext *f;
    int res;

    // wait for c->out_delayed[next] and move into c->out if visible
    Dav1dThreadPicture *out_delayed;
    if (c->n_fc > 1) {
        const unsigned next = c->frame_thread.next++;
        if (c->frame_thread.next == c->n_fc)
            c->frame_thread.next = 0;

        f = &c->fc[next];
        pthread_mutex_lock(&f->frame_thread.td.lock);
        while (f->n_tile_data > 0)
            pthread_cond_wait(&f->frame_thread.td.cond,
                              &f->frame_thread.td.lock);
        out_delayed = &c->frame_thread.out_delayed[next];
        if (out_delayed->p.data[0]) {
            if (out_delayed->visible && !out_delayed->flushed)
                dav1d_picture_ref(&c->out, &out_delayed->p);
            dav1d_thread_picture_unref(out_delayed);
        }
    } else {
        f = c->fc;
    }

    f->seq_hdr = c->seq_hdr;
    f->frame_hdr = c->frame_hdr;
    const int bd_idx = (f->seq_hdr.bpc - 8) >> 1;
    f->dsp = &c->dsp[bd_idx];

    if (!f->dsp->ipred.intra_pred[TX_4X4][DC_PRED]) {
        Dav1dDSPContext *const dsp = &c->dsp[bd_idx];

        switch (f->seq_hdr.bpc) {
#define assign_bitdepth_case(bd) \
        case bd: \
            dav1d_cdef_dsp_init_##bd##bpc(&dsp->cdef); \
            dav1d_intra_pred_dsp_init_##bd##bpc(&dsp->ipred); \
            dav1d_itx_dsp_init_##bd##bpc(&dsp->itx); \
            dav1d_loop_filter_dsp_init_##bd##bpc(&dsp->lf); \
            dav1d_loop_restoration_dsp_init_##bd##bpc(&dsp->lr); \
            dav1d_mc_dsp_init_##bd##bpc(&dsp->mc); \
            break
#if CONFIG_8BPC
        assign_bitdepth_case(8);
#endif
#if CONFIG_10BPC
        assign_bitdepth_case(10);
#endif
#undef assign_bitdepth_case
        default:
            fprintf(stderr, "Compiled without support for %d-bit decoding\n",
                    f->seq_hdr.bpc);
            return -ENOPROTOOPT;
        }
    }

#define assign_bitdepth_case(bd) \
        f->bd_fn.recon_b_inter = recon_b_inter_##bd##bpc; \
        f->bd_fn.recon_b_intra = recon_b_intra_##bd##bpc; \
        f->bd_fn.filter_sbrow = filter_sbrow_##bd##bpc; \
        f->bd_fn.backup_ipred_edge = backup_ipred_edge_##bd##bpc; \
        f->bd_fn.read_coef_blocks = read_coef_blocks_##bd##bpc
    if (f->seq_hdr.bpc <= 8) {
#if CONFIG_8BPC
        assign_bitdepth_case(8);
#endif
    } else {
#if CONFIG_10BPC
        assign_bitdepth_case(16);
#endif
    }
#undef assign_bitdepth_case

    if (f->frame_hdr.frame_type & 1)
        for (int i = 0; i < 7; i++) {
            const int refidx = f->frame_hdr.refidx[i];
            dav1d_thread_picture_ref(&f->refp[i], &c->refs[refidx].p);
        }

    // setup entropy
    if (f->frame_hdr.primary_ref_frame == PRIMARY_REF_NONE) {
        av1_init_states(&f->in_cdf, f->frame_hdr.quant.yac);
    } else {
        const int pri_ref = f->frame_hdr.refidx[f->frame_hdr.primary_ref_frame];
        cdf_thread_ref(&f->in_cdf, &c->cdf[pri_ref]);
    }
    if (f->frame_hdr.refresh_context) {
        cdf_thread_alloc(&f->out_cdf, c->n_fc > 1 ? &f->frame_thread.td : NULL);
        memcpy(f->out_cdf.cdf, f->in_cdf.cdf, sizeof(*f->in_cdf.cdf));
    }

    // FIXME qsort so tiles are in order (for frame threading)
    memcpy(f->tile, c->tile, c->n_tile_data * sizeof(*f->tile));
    f->n_tile_data = c->n_tile_data;
    c->n_tile_data = 0;

    // allocate frame
    if ((res = dav1d_thread_picture_alloc(&f->cur, f->frame_hdr.width,
                                          f->frame_hdr.height,
                                          f->seq_hdr.layout, f->seq_hdr.bpc,
                                          c->n_fc > 1 ? &f->frame_thread.td : NULL,
                                          f->frame_hdr.show_frame)) < 0)
    {
        return res;
    }

    f->cur.p.poc = f->frame_hdr.frame_offset;
    f->cur.p.p.type = f->frame_hdr.frame_type;
    f->cur.p.p.pri = f->seq_hdr.pri;
    f->cur.p.p.trc = f->seq_hdr.trc;
    f->cur.p.p.mtrx = f->seq_hdr.mtrx;
    f->cur.p.p.chr = f->seq_hdr.chr;
    f->cur.p.p.fullrange = f->seq_hdr.color_range;

    // move f->cur into output queue
    if (c->n_fc == 1) {
        if (f->frame_hdr.show_frame)
            dav1d_picture_ref(&c->out, &f->cur.p);
    } else {
        dav1d_thread_picture_ref(out_delayed, &f->cur);
    }

    f->bw = ((f->frame_hdr.width + 7) >> 3) << 1;
    f->bh = ((f->frame_hdr.height + 7) >> 3) << 1;
    f->sb128w = (f->bw + 31) >> 5;
    f->sb128h = (f->bh + 31) >> 5;
    f->sb_shift = 4 + f->seq_hdr.sb128;
    f->sb_step = 16 << f->seq_hdr.sb128;
    f->sbh = (f->bh + f->sb_step - 1) >> f->sb_shift;
    f->b4_stride = (f->bw + 31) & ~31;

    // ref_mvs
    if ((f->frame_hdr.frame_type & 1) || f->frame_hdr.allow_intrabc) {
        f->mvs_ref = dav1d_ref_create(f->sb128h * 32 * f->b4_stride *
                                      sizeof(*f->mvs));
        f->mvs = f->mvs_ref->data;
        if (f->frame_hdr.use_ref_frame_mvs) {
            for (int i = 0; i < 7; i++) {
                const int refidx = f->frame_hdr.refidx[i];
                f->refpoc[i] = f->refp[i].p.poc;
                if (c->refs[refidx].refmvs != NULL &&
                    f->refp[i].p.p.w == f->cur.p.p.w &&
                    f->refp[i].p.p.h == f->cur.p.p.h)
                {
                    f->ref_mvs_ref[i] = c->refs[refidx].refmvs;
                    dav1d_ref_inc(f->ref_mvs_ref[i]);
                    f->ref_mvs[i] = c->refs[refidx].refmvs->data;
                } else {
                    f->ref_mvs[i] = NULL;
                    f->ref_mvs_ref[i] = NULL;
                }
                memcpy(f->refrefpoc[i], c->refs[refidx].refpoc,
                       sizeof(*f->refrefpoc));
            }
        } else {
            memset(f->ref_mvs_ref, 0, sizeof(f->ref_mvs_ref));
        }
    } else {
        f->mvs_ref = NULL;
        memset(f->ref_mvs_ref, 0, sizeof(f->ref_mvs_ref));
    }

    // segmap
    if (f->frame_hdr.segmentation.enabled) {
        if (f->frame_hdr.segmentation.temporal) {
            const int pri_ref = f->frame_hdr.primary_ref_frame;
            assert(pri_ref != PRIMARY_REF_NONE);
            const int ref_w = (f->refp[pri_ref].p.p.w + 3) >> 2;
            const int ref_h = (f->refp[pri_ref].p.p.h + 3) >> 2;
            if (ref_w == f->bw && ref_h == f->bh) {
                f->prev_segmap_ref = c->refs[f->frame_hdr.refidx[pri_ref]].segmap;
                dav1d_ref_inc(f->prev_segmap_ref);
                f->prev_segmap = f->prev_segmap_ref->data;
            } else {
                f->prev_segmap_ref = NULL;
                f->prev_segmap = NULL;
            }
        } else {
            f->prev_segmap_ref = NULL;
            f->prev_segmap = NULL;
        }
        if (f->frame_hdr.segmentation.update_map) {
            f->cur_segmap_ref = dav1d_ref_create(f->b4_stride * 32 * f->sb128h);
            f->cur_segmap = f->cur_segmap_ref->data;
        } else {
            f->cur_segmap_ref = f->prev_segmap_ref;
            dav1d_ref_inc(f->cur_segmap_ref);
            f->cur_segmap = f->prev_segmap_ref->data;
        }
    } else {
        f->cur_segmap = NULL;
        f->cur_segmap_ref = NULL;
        f->prev_segmap_ref = NULL;
    }

    // update references etc.
    for (int i = 0; i < 8; i++) {
        if (f->frame_hdr.refresh_frame_flags & (1 << i)) {
            if (c->refs[i].p.p.data[0])
                dav1d_thread_picture_unref(&c->refs[i].p);
            dav1d_thread_picture_ref(&c->refs[i].p, &f->cur);

            if (c->cdf[i].cdf) cdf_thread_unref(&c->cdf[i]);
            if (f->frame_hdr.refresh_context) {
                cdf_thread_ref(&c->cdf[i], &f->out_cdf);
            } else {
                cdf_thread_ref(&c->cdf[i], &f->in_cdf);
            }
            c->refs[i].lf_mode_ref_deltas =
                f->frame_hdr.loopfilter.mode_ref_deltas;
            c->refs[i].seg_data = f->frame_hdr.segmentation.seg_data;
            memcpy(c->refs[i].gmv, f->frame_hdr.gmv, sizeof(c->refs[i].gmv));
            c->refs[i].film_grain = f->frame_hdr.film_grain.data;

            if (c->refs[i].segmap)
                dav1d_ref_dec(c->refs[i].segmap);
            c->refs[i].segmap = f->cur_segmap_ref;
            if (f->cur_segmap_ref)
                dav1d_ref_inc(f->cur_segmap_ref);
            if (c->refs[i].refmvs)
                dav1d_ref_dec(c->refs[i].refmvs);
            if (f->frame_hdr.allow_intrabc) {
                c->refs[i].refmvs = NULL;
            } else {
                c->refs[i].refmvs = f->mvs_ref;
                if (f->mvs_ref)
                    dav1d_ref_inc(f->mvs_ref);
            }
            memcpy(c->refs[i].refpoc, f->refpoc, sizeof(f->refpoc));
        }
    }

    if (c->n_fc == 1) {
        if ((res = decode_frame(f)) < 0)
            return res;
    } else {
        pthread_cond_signal(&f->frame_thread.td.cond);
        pthread_mutex_unlock(&f->frame_thread.td.lock);
    }

    return 0;
}
