/*
 * scale.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <string.h>

#include "dgx_scale_priv.h"
#include "dgx_vscreen_priv.h"

/*
 * A line is of indices, a byte a point, or of values, a word a point. The
 * few small helpers that touch the points are put into the code that calls
 * them, where the size is a constant, so the loops over the points of a
 * line are made once for each size; the rest takes the size as it comes.
 */
#define DGX_SCALE_INLINE static inline __attribute__((always_inline))

/* v / n rounded down, also left of 0: the point of the smaller picture a point of the larger one lies in */
static inline int32_t dgx_scale_div(int32_t v, int32_t n)
{
    return v >= 0 ? v / n : -((n - 1 - v) / n);
}

static inline int32_t dgx_scale_clamp(int32_t v, int32_t size)
{
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

DGX_SCALE_INLINE uint32_t dgx_scale_get(const void *line, size_t bytes, int32_t i)
{
    return bytes == 1 ? ((const uint8_t *)line)[i] : ((const uint32_t *)line)[i];
}

DGX_SCALE_INLINE void dgx_scale_put(void *line, size_t bytes, int32_t i, uint32_t value)
{
    if (bytes == 1) ((uint8_t *)line)[i] = (uint8_t)value;
    else ((uint32_t *)line)[i] = value;
}

/*
 * The neighbourhood of a point E of a line, with the lines one and two
 * before it, towards the top of the picture made larger, and after it:
 *
 *       J
 *     A B C
 *   K D E F L
 *     G H I
 *       M
 *
 * D is the point before E along the line, F the one after it. Scale2x and
 * Scale3x look at A to I, their SFX kin at all of them.
 */
typedef struct {
    uint32_t A, B, C, D, E, F, G, H, I, J, K, L, M;
} dgx_scale_around_t;

/* the neighbourhood of the point k of lines[2], lines[0 .. 4] being the lines two before it to two after it */
DGX_SCALE_INLINE void dgx_scale_around(dgx_scale_around_t *n, const void *const lines[5], size_t bytes, int32_t k, bool sfx)
{
    n->A = dgx_scale_get(lines[1], bytes, k - 1), n->B = dgx_scale_get(lines[1], bytes, k), n->C = dgx_scale_get(lines[1], bytes, k + 1);
    n->D = dgx_scale_get(lines[2], bytes, k - 1), n->E = dgx_scale_get(lines[2], bytes, k), n->F = dgx_scale_get(lines[2], bytes, k + 1);
    n->G = dgx_scale_get(lines[3], bytes, k - 1), n->H = dgx_scale_get(lines[3], bytes, k), n->I = dgx_scale_get(lines[3], bytes, k + 1);
    if (!sfx) return;
    n->J = dgx_scale_get(lines[0], bytes, k), n->K = dgx_scale_get(lines[2], bytes, k - 2);
    n->L = dgx_scale_get(lines[2], bytes, k + 2), n->M = dgx_scale_get(lines[4], bytes, k);
}

/*
 * Scale2x of a point: the two points of its first half, the one towards B,
 * into out[0] and out[1], and of its second half into out[2] and out[3].
 * A corner where two neighbours meet equal, and the two across from them
 * differ, takes their pixel: the steps of a slanted edge are filled in.
 * Scale2xSFX does not fill a corner when the pixel beyond it is E and that
 * diagonal goes no further, which keeps a thin slanted line from growing
 * lumps. Where B and H, or D and F, are equal all four points are E: most
 * of a picture, which dgx_scale_block2() passes over.
 */
static inline void dgx_scale_corners2(const dgx_scale_around_t *n, bool sfx, uint32_t out[4])
{
    const uint32_t B = n->B, D = n->D, E = n->E, F = n->F, H = n->H;
    if (!sfx) {
        out[0] = D == B ? D : E;
        out[1] = B == F ? F : E;
        out[2] = D == H ? D : E;
        out[3] = H == F ? F : E;
        return;
    }
    const uint32_t A = n->A, C = n->C, G = n->G, I = n->I;
    const bool     par0 = B != F && D != H, par1 = B != D && F != H;
    const bool     AE = A == E, CE = C == E, EG = E == G, EI = E == I;
    const bool     art0 = CE || EG, art1 = AE || EI;
    out[0] = B == D && par0 && (!AE || art0 || A == n->J || A == n->K) ? B : E;
    out[1] = B == F && par1 && (!CE || art1 || C == n->J || C == n->L) ? F : E;
    out[2] = D == H && par1 && (!EG || art1 || G == n->K || G == n->M) ? D : E;
    out[3] = F == H && par0 && (!EI || art0 || I == n->L || I == n->M) ? F : E;
}

/* the same, but false and nothing put where B and H, or D and F, are equal: all four points are E there */
static inline bool dgx_scale_block2(const dgx_scale_around_t *n, bool sfx, uint32_t out[4])
{
    if (n->B == n->H || n->D == n->F) return false;
    dgx_scale_corners2(n, sfx, out);
    return true;
}

/*
 * Scale3x of a point: three rows of three points into out[0 .. 8]. The
 * corners are filled as by Scale2x; a point between two corners takes the
 * pixel too where the corner on its other side does not fill in. Scale3xSFX
 * keeps the corners of Scale2xSFX and fills the corner of a 45-degree edge
 * as well. Returns false when all nine points are E.
 */
static inline bool dgx_scale_block3(const dgx_scale_around_t *n, bool sfx, uint32_t out[9])
{
    const uint32_t A = n->A, B = n->B, C = n->C, D = n->D, E = n->E, F = n->F, G = n->G, H = n->H, I = n->I;
    if (!sfx) {
        if (B == H || D == F) return false;
        const bool BD = B == D, BF = B == F, HD = H == D, HF = H == F;
        out[0] = BD ? B : E;
        out[1] = (BD && E != C) || (BF && E != A) ? B : E;
        out[2] = BF ? B : E;
        out[3] = (BD && E != G) || (HD && E != A) ? D : E;
        out[4] = E;
        out[5] = (BF && E != I) || (HF && E != C) ? F : E;
        out[6] = HD ? H : E;
        out[7] = (HD && E != I) || (HF && E != G) ? H : E;
        out[8] = HF ? H : E;
        return true;
    }
    /* every point takes B, D, F or H where it is not E: nothing to do where they are all E, nor where no two of them are equal */
    const bool BD = B == D, BF = B == F, DH = D == H, FH = F == H;
    if ((B == E && D == E && F == E && H == E) || !(BD || BF || DH || FH)) return false;
    const uint32_t J = n->J, K = n->K, L = n->L, M = n->M;
    const bool     AE = A == E, CE = C == E, EG = E == G, EI = E == I;
    const bool     par0 = !(BF || DH), par1 = !(BD || FH);
    const bool     dia0 = AE || EI, dia1 = CE || EG;
    const bool     art0 = BD && par0 && (!AE || dia1 || A == J || A == K);
    const bool     art2 = BF && par1 && (!CE || dia0 || C == J || C == L);
    const bool     art6 = DH && par1 && (!EG || dia0 || G == K || G == M);
    const bool     art8 = FH && par0 && (!EI || dia1 || I == L || I == M);
    out[0] = art0 || (BD && ((CE && C != J && !AE) || (EG && !AE && G != K))) ? B : E;
    out[1] = (art0 && !CE) || (art2 && !AE) ? B : E;
    out[2] = art2 || (BF && ((AE && A != J && !CE) || (EI && !CE && I != L))) ? B : E;
    out[3] = (art0 && !EG) || (art6 && !AE) ? D : E;
    out[4] = E;
    out[5] = (art8 && !CE) || (art2 && !EI) ? F : E;
    out[6] = art6 || (DH && ((AE && A != K && !EG) || (EI && !EG && I != M))) ? H : E;
    out[7] = (art8 && !EG) || (art6 && !EI) ? H : E;
    out[8] = art8 || (FH && ((CE && C != L && !EI) || (EG && !EI && G != M))) ? H : E;
    return true;
}

/*
 * The points from .. from + count - 1 of the n lines (2 or 3) that a pass
 * makes of lines[2], the line of the smaller picture, into rows[0 .. n - 1],
 * the line towards lines[1] first. lines[0 .. 4] are the lines two before
 * it to two after it, their point p at [p - base]; only SFX looks at the
 * outer two.
 */
DGX_SCALE_INLINE void dgx_scale_rows_of(void *const rows[3], size_t bytes, int n, bool sfx, const void *const lines[5], int32_t base, int32_t from,
                                        uint32_t count)
{
    const int32_t last = from + (int32_t)count - 1;
    int32_t       k = dgx_scale_div(from, n), at = 0;
    int           first = (int)(from - k * n);
    for (; k * n <= last; ++k, first = 0) {
        /* the points of this one of the smaller picture the lines take: first .. end of its n */
        const int          end = k * n + n - 1 <= last ? n - 1 : (int)(last - k * n);
        dgx_scale_around_t around;
        dgx_scale_around(&around, lines, bytes, k - base, sfx);
        uint32_t   block[9];
        const bool edge = n == 2 ? dgx_scale_block2(&around, sfx, block) : dgx_scale_block3(&around, sfx, block);
        for (int j = 0; j < n; ++j) {
            for (int s = first; s <= end; ++s) dgx_scale_put(rows[j], bytes, at + s - first, edge ? block[j * n + s] : around.E);
        }
        at += end - first + 1;
    }
}

static void dgx_scale_rows8(void *const rows[3], int n, bool sfx, const void *const lines[5], int32_t base, int32_t from, uint32_t count)
{
    dgx_scale_rows_of(rows, 1, n, sfx, lines, base, from, count);
}

static void dgx_scale_rows32(void *const rows[3], int n, bool sfx, const void *const lines[5], int32_t base, int32_t from, uint32_t count)
{
    dgx_scale_rows_of(rows, 4, n, sfx, lines, base, from, count);
}

static inline void dgx_scale_rows(void *const rows[3], size_t bytes, int n, bool sfx, const void *const lines[5], int32_t base, int32_t from,
                                  uint32_t count)
{
    if (bytes == 1) dgx_scale_rows8(rows, n, sfx, lines, base, from, count);
    else dgx_scale_rows32(rows, n, sfx, lines, base, from, count);
}

/*
 * Scale3x or Scale3xSFX straight into colors of a table, for a display:
 * the three rows a line of the smaller picture makes, rows[0] towards the
 * top, a point `step` pixels after the one before it, -1 for a row made
 * from its end backwards. A point with no edge, most of a picture, puts its
 * nine pixels at once; a point cut by an end of the line goes by its block.
 */
DGX_SCALE_INLINE void dgx_scale_triple_colors(uint16_t *const rows[3], ptrdiff_t step, const void *const lines[5], bool sfx, int32_t base,
                                              int32_t from, uint32_t count, const uint16_t *restrict lut)
{
    const uint8_t *restrict up = lines[1], *restrict mid = lines[2], *restrict down = lines[3];
    const int32_t last = from + (int32_t)count - 1, k_last = dgx_scale_div(last, 3);
    int32_t       k = dgx_scale_div(from, 3);
    uint16_t     *p0 = rows[0], *p1 = rows[1], *p2 = rows[2];
    /* the points first .. end of the block of k, looked at whole */
#define DGX_SCALE_PART(first, end)                                                                                \
    do {                                                                                                          \
        dgx_scale_around_t around;                                                                                \
        dgx_scale_around(&around, lines, 1, k - base, sfx);                                                       \
        uint32_t   block[9];                                                                                      \
        const bool edge = dgx_scale_block3(&around, sfx, block);                                                  \
        for (int s_ = (first); s_ <= (end); ++s_, p0 += step, p1 += step, p2 += step) {                           \
            *p0 = lut[edge ? block[s_] : around.E];                                                               \
            *p1 = lut[edge ? block[3 + s_] : around.E];                                                           \
            *p2 = lut[edge ? block[6 + s_] : around.E];                                                           \
        }                                                                                                         \
    } while (0)
    const int head = (int)(from - 3 * k);
    if (head || k == k_last) {
        DGX_SCALE_PART(head, k == k_last ? (int)(last - 3 * k) : 2);
        if (k == k_last) return;
        ++k;
    }
    /* the whole blocks, d e f sliding along the middle line */
    const int32_t whole_last = 3 * k_last + 2 == last ? k_last : k_last - 1;
    int32_t       i = k - base;
    uint8_t       d = mid[i - 1], e = mid[i], f = mid[i + 1];
    for (; k <= whole_last; ++k, ++i, d = e, e = f, f = mid[i + 1]) {
        const uint8_t b = up[i], h = down[i];
        /* nothing but E comes out where Scale3x finds no edge, and where SFX finds the four around equal to E or no two of them equal */
        const bool flat = !sfx ? b == h || d == f : (b == e && d == e && f == e && h == e) || !(b == d || b == f || d == h || f == h);
        if (flat) {
            const uint16_t c = lut[e];
            p0[0] = p0[step] = p0[2 * step] = c;
            p1[0] = p1[step] = p1[2 * step] = c;
            p2[0] = p2[step] = p2[2 * step] = c;
        } else {
            dgx_scale_around_t around;
            dgx_scale_around(&around, lines, 1, i, sfx);
            uint32_t block[9];
            if (!dgx_scale_block3(&around, sfx, block))
                for (int j = 0; j < 9; ++j) block[j] = e;
            p0[0] = lut[block[0]], p0[step] = lut[block[1]], p0[2 * step] = lut[block[2]];
            p1[0] = lut[block[3]], p1[step] = lut[block[4]], p1[2 * step] = lut[block[5]];
            p2[0] = lut[block[6]], p2[step] = lut[block[7]], p2[2 * step] = lut[block[8]];
        }
        p0 += 3 * step, p1 += 3 * step, p2 += 3 * step;
    }
    if (k == k_last) DGX_SCALE_PART(0, (int)(last - 3 * k));
#undef DGX_SCALE_PART
}

/*
 * Scale2x or Scale2xSFX of a line of indices, both halves at once, into
 * `top` and `bottom`: the picture twice as large that 4 is made from, and
 * the lines of a virtual screen. A point with no edge, most of a picture,
 * is put at once; only the others look at their neighbours.
 */
DGX_SCALE_INLINE void dgx_scale_index_pair(uint8_t *restrict top, uint8_t *restrict bottom, const void *const lines[5], bool sfx, int32_t base,
                                           int32_t from, uint32_t count)
{
    if (!count) return;
    const uint8_t *restrict up = lines[1], *restrict mid = lines[2], *restrict down = lines[3];
    uint32_t k = (uint32_t)(dgx_scale_div(from, 2) - base);
    uint8_t  d = mid[k - 1], e = mid[k], f = mid[k + 1];
    uint32_t left = count;
    if (from & 1) {
        /* the line starts with the second point of a pair */
        dgx_scale_around_t around;
        dgx_scale_around(&around, lines, 1, (int32_t)k, sfx);
        uint32_t   block[4];
        const bool edge = dgx_scale_block2(&around, sfx, block);
        *top++          = (uint8_t)(edge ? block[1] : e);
        *bottom++       = (uint8_t)(edge ? block[3] : e);
        --left;
        ++k;
        d = e, e = f, f = mid[k + 1];
    }
    for (; left >= 2; left -= 2, ++k, d = e, e = f, f = mid[k + 1]) {
        const uint8_t b = up[k], h = down[k];
        if (b == h || d == f) {
            top[0] = top[1] = bottom[0] = bottom[1] = e;
        } else if (!sfx) {
            top[0]    = d == b ? d : e;
            top[1]    = b == f ? f : e;
            bottom[0] = d == h ? d : e;
            bottom[1] = h == f ? f : e;
        } else {
            dgx_scale_around_t around;
            dgx_scale_around(&around, lines, 1, (int32_t)k, true);
            uint32_t block[4];
            dgx_scale_corners2(&around, true, block);
            top[0] = (uint8_t)block[0], top[1] = (uint8_t)block[1];
            bottom[0] = (uint8_t)block[2], bottom[1] = (uint8_t)block[3];
        }
        top += 2, bottom += 2;
    }
    if (left) {
        /* the line ends with the first point of a pair */
        dgx_scale_around_t around;
        dgx_scale_around(&around, lines, 1, (int32_t)k, sfx);
        uint32_t   block[4];
        const bool edge = dgx_scale_block2(&around, sfx, block);
        *top            = (uint8_t)(edge ? block[0] : e);
        *bottom         = (uint8_t)(edge ? block[2] : e);
    }
}

static void dgx_scale_plain_pair(uint8_t *top, uint8_t *bottom, const void *const lines[5], int32_t base, int32_t from, uint32_t count)
{
    dgx_scale_index_pair(top, bottom, lines, false, base, from, count);
}

static void dgx_scale_sfx_pair(uint8_t *top, uint8_t *bottom, const void *const lines[5], int32_t base, int32_t from, uint32_t count)
{
    dgx_scale_index_pair(top, bottom, lines, true, base, from, count);
}

/* a word of two pixels, where the pointer is on a boundary of a word */
static inline void dgx_scale_put_word(uint32_t *to, uint32_t word)
{
    memcpy(to, &word, sizeof(word));
}

/*
 * The same as dgx_scale_plain_pair() straight into colors: half 0 into
 * `top`, half 1 into `bottom`, a point `step` pixels from the one before it
 * (-1 for a line made backwards). With `sfx` the corners are those of
 * Scale2xSFX, which looks at lines[0] and lines[4] too.
 */
DGX_SCALE_INLINE void dgx_scale_pair_colors(uint16_t *restrict top, uint16_t *restrict bottom, ptrdiff_t step, const void *const lines[5], bool sfx,
                                            int32_t base, int32_t from, uint32_t count, const uint16_t *restrict lut)
{
    if (!count) return;
    const uint8_t *restrict up = lines[1], *restrict mid = lines[2], *restrict down = lines[3];
    uint32_t k = (uint32_t)(dgx_scale_div(from, 2) - base);
    uint8_t  d = mid[k - 1], e = mid[k], f = mid[k + 1];
    uint32_t left = count;
    if (from & 1) {
        /* the line starts with the second point of a pair */
        dgx_scale_around_t around;
        dgx_scale_around(&around, lines, 1, (int32_t)k, sfx);
        uint32_t   block[4];
        const bool edge = dgx_scale_block2(&around, sfx, block);
        *top            = lut[edge ? block[1] : e];
        *bottom         = lut[edge ? block[3] : e];
        top += step, bottom += step;
        --left;
        ++k;
        d = e, e = f, f = mid[k + 1];
    }
    /*
     * The two points of a pair lie side by side in memory, the first one at
     * the higher address when the line is made backwards. Where both rows
     * have a pair on a boundary of a word, a pair goes as one word.
     */
    uint32_t *word_top = (uint32_t *)(step > 0 ? top : top - 1), *word_bottom = (uint32_t *)(step > 0 ? bottom : bottom - 1);
    if (left >= 2 && !(((uintptr_t)word_top | (uintptr_t)word_bottom) & 3u)) {
        /* where the first point and the second go in a word: the low half is the lower address */
        const unsigned first_at = step > 0 ? 0u : 16u, second_at = 16u - first_at;
        for (; left >= 2; left -= 2, ++k, d = e, e = f, f = mid[k + 1]) {
            const uint8_t  b = up[k], h = down[k];
            const uint32_t c = lut[e];
            if (b == h || d == f) {
                /* no edge here: most of a picture */
                const uint32_t both = c | c << 16;
                dgx_scale_put_word(word_top, both);
                dgx_scale_put_word(word_bottom, both);
            } else if (!sfx) {
                const uint32_t cd = lut[d], cf = lut[f];
                dgx_scale_put_word(word_top, (d == b ? cd : c) << first_at | (b == f ? cf : c) << second_at);
                dgx_scale_put_word(word_bottom, (d == h ? cd : c) << first_at | (h == f ? cf : c) << second_at);
            } else {
                dgx_scale_around_t around;
                dgx_scale_around(&around, lines, 1, (int32_t)k, true);
                uint32_t block[4];
                dgx_scale_corners2(&around, true, block);
                dgx_scale_put_word(word_top, (uint32_t)lut[block[0]] << first_at | (uint32_t)lut[block[1]] << second_at);
                dgx_scale_put_word(word_bottom, (uint32_t)lut[block[2]] << first_at | (uint32_t)lut[block[3]] << second_at);
            }
            word_top += step, word_bottom += step;
        }
        top    = (uint16_t *)word_top + (step > 0 ? 0 : 1);
        bottom = (uint16_t *)word_bottom + (step > 0 ? 0 : 1);
    }
    for (; left >= 2; left -= 2, ++k, d = e, e = f, f = mid[k + 1]) {
        const uint8_t  b = up[k], h = down[k];
        const uint16_t c = lut[e];
        if (b == h || d == f) {
            /* no edge here: most of a picture */
            top[0] = top[step] = bottom[0] = bottom[step] = c;
        } else if (!sfx) {
            const uint16_t cd = lut[d], cf = lut[f];
            top[0]            = d == b ? cd : c;
            top[step]         = b == f ? cf : c;
            bottom[0]         = d == h ? cd : c;
            bottom[step]      = h == f ? cf : c;
        } else {
            dgx_scale_around_t around;
            dgx_scale_around(&around, lines, 1, (int32_t)k, true);
            uint32_t block[4];
            dgx_scale_corners2(&around, true, block);
            top[0] = lut[block[0]], top[step] = lut[block[1]];
            bottom[0] = lut[block[2]], bottom[step] = lut[block[3]];
        }
        top += 2 * step, bottom += 2 * step;
    }
    if (left) {
        /* the line ends with the first point of a pair */
        dgx_scale_around_t around;
        dgx_scale_around(&around, lines, 1, (int32_t)k, sfx);
        uint32_t   block[4];
        const bool edge = dgx_scale_block2(&around, sfx, block);
        *top            = lut[edge ? block[0] : e];
        *bottom         = lut[edge ? block[2] : e];
    }
}

/*
 * The points of the smaller picture a pass n times as large needs for the
 * points from .. from + count - 1 of its lines, with two more on either
 * side for the neighbours of the outermost ones.
 */
static void dgx_scale_span(int32_t from, uint32_t count, int32_t n, int32_t *base, uint32_t *len)
{
    *base = dgx_scale_div(from, n) - 2;
    *len  = (uint32_t)(dgx_scale_div(from + (int32_t)count - 1, n) + 2 - *base + 1);
}

/* the scratch of the lines, in this order: rows of colors not asked for, the lines made last, the kept lines of the two pictures */
typedef struct {
    size_t   spare, made, kept0, kept1;
    uint32_t len0, len1; /* points a kept line may have at most */
    uint8_t  block;
    bool     smoothed;
} dgx_scale_layout_t;

static inline size_t dgx_scale_words(size_t bytes)
{
    return (bytes + 3u) & ~(size_t)3u;
}

static void dgx_scale_layout(uint8_t scale, uint8_t smooth, uint32_t count, size_t bytes, dgx_scale_layout_t *l)
{
    if (!scale) scale = 1;
    l->smoothed = smooth && (scale == 2 || scale == 3 || scale == 4);
    l->block    = l->smoothed ? (scale == 3 ? 3 : 2) : scale;
    /* dgx_scale_span() of any `from` takes at most so many points */
    if (l->smoothed && scale == 4) {
        l->len1 = count / 2u + 6u;
        l->len0 = l->len1 / 2u + 6u;
    } else {
        l->len1 = 0;
        l->len0 = count / l->block + 6u;
    }
    l->spare = bytes == 1 ? dgx_scale_words(2u * 2u * (size_t)count) : 0;
    l->made  = dgx_scale_words((l->smoothed ? l->block : 1u) * (size_t)count * bytes);
    l->kept0 = 8u * dgx_scale_words((size_t)l->len0 * bytes);
    l->kept1 = 8u * dgx_scale_words((size_t)l->len1 * bytes);
}

size_t dgx_scale_scratch(uint8_t scale, uint8_t smooth, uint32_t count, size_t bytes)
{
    dgx_scale_layout_t l;
    dgx_scale_layout(scale, smooth, count, bytes, &l);
    return l.spare + l.made + l.kept0 + l.kept1;
}

static void dgx_scale_begin(dgx_scale_lines_t *lines, size_t bytes, uint16_t width, uint16_t height, uint8_t scale, uint8_t smooth, bool along_y,
                            int32_t from, uint32_t count, uint8_t *scratch)
{
    dgx_scale_layout_t l;
    dgx_scale_layout(scale, smooth, count, bytes, &l);
    lines->width   = width;
    lines->height  = height;
    lines->scale   = scale ? scale : 1;
    lines->smooth  = l.smoothed ? smooth : 0;
    lines->block   = l.block;
    lines->along_y = along_y;
    lines->from    = from;
    lines->count   = count;
    if (l.smoothed && lines->scale == 4) {
        /* the points of the picture twice as large the last pass needs, and of the picture itself those need */
        dgx_scale_span(from, count, 2, &lines->base[1], &lines->len[1]);
        dgx_scale_span(lines->base[1], lines->len[1], 2, &lines->base[0], &lines->len[0]);
    } else {
        dgx_scale_span(from, count, lines->block, &lines->base[0], &lines->len[0]);
        lines->base[1] = 0, lines->len[1] = 0;
    }
    lines->spare[0] = (uint16_t *)scratch;
    lines->spare[1] = lines->spare[0] + count;
    scratch += l.spare;
    for (int j = 0; j < 3; ++j) lines->made[j] = scratch + (j < (l.smoothed ? l.block : 1) ? (size_t)j * count * bytes : 0u);
    scratch += l.made;
    lines->made_from = INT32_MIN;
    for (int i = 0; i < 8; ++i) {
        lines->tag[0][i]  = INT32_MIN;
        lines->tag[1][i]  = INT32_MIN;
        lines->line[0][i] = scratch + (size_t)i * (l.kept0 / 8u);
        lines->line[1][i] = scratch + l.kept0 + (size_t)i * (l.kept1 / 8u);
    }
}

void dgx_scale_lines_begin(dgx_scale_lines_t *lines, const dgx_bus_indexed_t *picture, bool along_y, int32_t from, uint32_t count, uint8_t *scratch)
{
    lines->picture = picture;
    memset(&lines->source, 0, sizeof(lines->source));
    dgx_scale_begin(lines, 1, picture->width, picture->height, picture->scale, picture->smooth, along_y, from, count, scratch);
}

void dgx_scale_values_begin(dgx_scale_lines_t *lines, const dgx_scale_source_t *source, uint8_t scale, uint8_t smooth, bool along_y, int32_t from,
                            uint32_t count, uint8_t *scratch)
{
    lines->picture = NULL;
    lines->source  = *source;
    dgx_scale_begin(lines, 4, (uint16_t)source->screen->width, (uint16_t)source->screen->height, scale, smooth, along_y, from, count, scratch);
}

/* the values of the line c of a source, its points base .. base + len - 1 cut by `run`, the length of a line */
static void dgx_scale_fetch_values(const dgx_scale_source_t *source, bool along_y, int32_t c, int32_t base, uint32_t len, int32_t run, uint32_t *out)
{
    for (uint32_t i = 0; i < len; ++i) {
        const int32_t p = dgx_scale_clamp(base + (int32_t)i, run), x = along_y ? c : p, y = along_y ? p : c;
        if (!source->memory) {
            out[i] = source->screen->get_pixel((dgx_screen_t *)source->screen, x, y);
            continue;
        }
        out[i] = dgx_vscreen_value(source->memory + (size_t)y * source->pitch + (size_t)x * source->bytes, source->bytes);
    }
}

/* the line t of the picture itself made into its slot, its points base[0] .. base[0] + len[0] - 1; past its edges its last pixel goes on */
static const void *dgx_scale_make0(dgx_scale_lines_t *lines, size_t bytes, int32_t t)
{
    void *line = lines->line[0][t & 7];
    lines->tag[0][t & 7] = t;
    const int32_t run    = lines->along_y ? lines->height : lines->width;
    const int32_t c      = dgx_scale_clamp(t, lines->along_y ? lines->width : lines->height);
    if (bytes == 1) {
        const dgx_bus_indexed_t *picture = lines->picture;
        const uint32_t           step    = lines->along_y ? picture->pitch : 1u;
        const uint8_t           *first   = picture->pixels + (size_t)c * (lines->along_y ? 1u : picture->pitch);
        uint8_t                 *out     = line;
        for (uint32_t i = 0; i < lines->len[0]; ++i) out[i] = first[(size_t)dgx_scale_clamp(lines->base[0] + (int32_t)i, run) * step];
    } else {
        dgx_scale_fetch_values(&lines->source, lines->along_y, c, lines->base[0], lines->len[0], run, line);
    }
    return line;
}

/* the line t of the picture itself: kept, or made now */
static inline const void *dgx_scale_kept0(dgx_scale_lines_t *lines, size_t bytes, int32_t t)
{
    return lines->tag[0][t & 7] == t ? lines->line[0][t & 7] : dgx_scale_make0(lines, bytes, t);
}

/* the five lines of the picture itself around its line t; plain smoothing does not look at the outer two, and they are not made */
static void dgx_scale_around0(dgx_scale_lines_t *lines, size_t bytes, bool sfx, int32_t t, const void *around[5])
{
    around[1] = dgx_scale_kept0(lines, bytes, t - 1);
    around[2] = dgx_scale_kept0(lines, bytes, t);
    around[3] = dgx_scale_kept0(lines, bytes, t + 1);
    around[0] = sfx ? dgx_scale_kept0(lines, bytes, t - 2) : around[1];
    around[4] = sfx ? dgx_scale_kept0(lines, bytes, t + 2) : around[3];
}

/*
 * The line t of the picture twice as large, at 4. It is made with the other
 * line of its pair into the slots next to each other: the five lines in a
 * row a line of the larger picture needs lie in three pairs, in six of the
 * eight slots, and none of them is put out by another while they are used.
 */
static const void *dgx_scale_make1(dgx_scale_lines_t *lines, size_t bytes, int32_t t)
{
    void *line = lines->line[1][t & 7];
    const int32_t half = dgx_scale_div(t, 2), even = 2 * half;
    lines->tag[1][even & 7]       = even;
    lines->tag[1][(even + 1) & 7] = even + 1;
    const bool  sfx               = lines->smooth == DGX_BUS_SMOOTH_SFX;
    const void *around[5];
    dgx_scale_around0(lines, bytes, sfx, half, around);
    void *const rows[3] = {lines->line[1][even & 7], lines->line[1][(even + 1) & 7], NULL};
    if (bytes == 1 && !sfx) dgx_scale_plain_pair(rows[0], rows[1], around, lines->base[0], lines->base[1], lines->len[1]);
    else if (bytes == 1) dgx_scale_sfx_pair(rows[0], rows[1], around, lines->base[0], lines->base[1], lines->len[1]);
    else dgx_scale_rows(rows, bytes, 2, sfx, around, lines->base[0], lines->base[1], lines->len[1]);
    return line;
}

/* the line t of the picture twice as large: kept, or made now with the other line of its pair */
static inline const void *dgx_scale_kept1(dgx_scale_lines_t *lines, size_t bytes, int32_t t)
{
    return lines->tag[1][t & 7] == t ? lines->line[1][t & 7] : dgx_scale_make1(lines, bytes, t);
}

/* the five lines the last pass makes the lines of block b from, and the point at [0] of them */
static inline int32_t dgx_scale_around_last(dgx_scale_lines_t *lines, size_t bytes, int32_t b, const void *around[5])
{
    const bool sfx = lines->smooth == DGX_BUS_SMOOTH_SFX;
    if (lines->scale != 4) {
        dgx_scale_around0(lines, bytes, sfx, b, around);
        return lines->base[0];
    }
    around[1] = dgx_scale_kept1(lines, bytes, b - 1);
    around[2] = dgx_scale_kept1(lines, bytes, b);
    around[3] = dgx_scale_kept1(lines, bytes, b + 1);
    around[0] = sfx ? dgx_scale_kept1(lines, bytes, b - 2) : around[1];
    around[4] = sfx ? dgx_scale_kept1(lines, bytes, b + 2) : around[3];
    return lines->base[1];
}

/* the points of `line`, the point p at [p - base], each repeated n times, into the `count` points of `out` from `from` on */
static void dgx_scale_repeat(void *out, size_t bytes, const void *line, int32_t base, int32_t from, uint32_t count, int32_t n)
{
    int32_t k   = dgx_scale_div(from, n);
    int32_t run = (k + 1) * n - from; /* what is left of the first square */
    for (uint32_t i = 0; i < count; ++k, run = n) {
        const uint32_t value = dgx_scale_get(line, bytes, k - base);
        for (; run > 0 && i < count; --run, ++i) dgx_scale_put(out, bytes, (int32_t)i, value);
    }
}

static const void *dgx_scale_line_of(dgx_scale_lines_t *lines, size_t bytes, int32_t at)
{
    const int32_t n = lines->block, b = dgx_scale_div(at, n);
    if (b != lines->made_from) {
        lines->made_from = b;
        if (!lines->smooth) {
            /* every point a square: the line of the picture it lies in, each point repeated */
            dgx_scale_repeat(lines->made[0], bytes, dgx_scale_kept0(lines, bytes, b), lines->base[0], lines->from, lines->count, n);
        } else {
            const void   *around[5];
            const int32_t base = dgx_scale_around_last(lines, bytes, b, around);
            const bool    sfx  = lines->smooth == DGX_BUS_SMOOTH_SFX;
            if (n == 2 && bytes == 1 && !sfx) {
                dgx_scale_plain_pair(lines->made[0], lines->made[1], around, base, lines->from, lines->count);
            } else if (n == 2 && bytes == 1) {
                dgx_scale_sfx_pair(lines->made[0], lines->made[1], around, base, lines->from, lines->count);
            } else {
                dgx_scale_rows(lines->made, bytes, n, sfx, around, base, lines->from, lines->count);
            }
        }
    }
    return lines->smooth ? lines->made[at - b * n] : lines->made[0];
}

const uint8_t *dgx_scale_line(dgx_scale_lines_t *lines, int32_t at)
{
    return dgx_scale_line_of(lines, 1, at);
}

const uint32_t *dgx_scale_values(dgx_scale_lines_t *lines, int32_t at)
{
    return dgx_scale_line_of(lines, 4, at);
}

void dgx_scale_colors(dgx_scale_lines_t *lines, int32_t at, int dir, uint32_t asked, uint16_t *row, ptrdiff_t pitch, ptrdiff_t step)
{
    const bool      sfx = lines->smooth == DGX_BUS_SMOOTH_SFX;
    const uint16_t *lut = lines->picture->lut;
    const void     *around[5];
    if (lines->block == 2) {
        /* a pair, the usual block, without a division: the line asked first, and its other one, asked too or not */
        uint16_t     *pair[2];
        const int32_t j = at & 1;
        pair[j]         = row;
        pair[j ^ 1]     = asked == 2 ? row + pitch : lines->spare[0] + (step < 0 ? lines->count - 1u : 0u);
        const int32_t base = dgx_scale_around_last(lines, 1, dgx_scale_div(at, 2), around);
        if (sfx) dgx_scale_pair_colors(pair[0], pair[1], step, around, true, base, lines->from, lines->count, lut);
        else dgx_scale_pair_colors(pair[0], pair[1], step, around, false, base, lines->from, lines->count, lut);
        return;
    }
    const int32_t n = lines->block, b = dgx_scale_div(at, n), j = at - b * n;
    /* the lines asked for go to their rows, the others of the block to rows nobody looks at */
    uint16_t *rows[3] = {NULL, NULL, NULL};
    for (uint32_t i = 0; i < asked; ++i) rows[j + (int32_t)i * dir] = row + (ptrdiff_t)i * pitch;
    for (int r = 0, s = 0; r < n; ++r) {
        if (!rows[r]) rows[r] = lines->spare[s++] + (step < 0 ? lines->count - 1u : 0u);
    }
    const int32_t base = dgx_scale_around_last(lines, 1, b, around);
    if (sfx) dgx_scale_triple_colors(rows, step, around, true, base, lines->from, lines->count, lut);
    else dgx_scale_triple_colors(rows, step, around, false, base, lines->from, lines->count, lut);
}
