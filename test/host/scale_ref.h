#pragma once
/*
 * Scale2x, Scale2xSFX, Scale3x and Scale3xSFX as the shaders of libretro
 * write them, a point at a time, for the tests to hold the smoothing of DGX
 * against. Nothing here is fast; every point of the larger picture is worked
 * out from its neighbours on its own.
 *
 * The rules are those of edge-smoothing/scalenx in libretro/slang-shaders
 * (scale2x, scale2xSFX, scale3x) and of scalenx/shaders/scale3xSFX.cg in
 * libretro/common-shaders: the slang and GLSL files named scale3xSFX are a
 * copy of scale2xSFX. Pixels are equal when their values are, with no
 * threshold, and a mix of two equal colors is that color.
 *
 * The picture goes on past its edges by its last pixel. At 4 the picture is
 * made twice as large twice: the larger picture of the first pass is worked
 * out from the picture so continued, everywhere the second pass looks, past
 * its edges too.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const uint32_t *pixels; /* row after row, a value a pixel */
    int             width, height;
} ref_picture_t;

enum { REF_BLOCKS = 0, REF_SMOOTH = 1, REF_SFX = 2 };

static inline int ref_floor_div(int v, int n)
{
    return v >= 0 ? v / n : -((n - 1 - v) / n);
}

static inline uint32_t ref_at(const ref_picture_t *p, int x, int y)
{
    x = x < 0 ? 0 : x >= p->width ? p->width - 1 : x;
    y = y < 0 ? 0 : y >= p->height ? p->height - 1 : y;
    return p->pixels[(size_t)y * p->width + x];
}

/*
 *     J
 *   A B C
 * K D E F L
 *   G H I
 *     M
 */
typedef struct {
    uint32_t A, B, C, D, E, F, G, H, I, J, K, L, M;
} ref_around_t;

static void ref_scale2x(const ref_around_t *n, bool sfx, uint32_t out[2][2])
{
    const uint32_t A = n->A, B = n->B, C = n->C, D = n->D, E = n->E, F = n->F, G = n->G, H = n->H, I = n->I;
    const uint32_t J = n->J, K = n->K, L = n->L, M = n->M;
    if (!sfx) {
        const bool edge = B != H && D != F;
        out[0][0]       = edge && B == D ? B : E;
        out[0][1]       = edge && B == F ? B : E;
        out[1][0]       = edge && H == D ? H : E;
        out[1][1]       = edge && H == F ? H : E;
        return;
    }
    const bool par0 = B != F && D != H, par1 = B != D && F != H;
    const bool AE = A == E, CE = C == E, EG = E == G, EI = E == I;
    const bool art0 = CE || EG, art1 = AE || EI;
    out[0][0] = B == D && par0 && (!AE || art0 || A == J || A == K) ? B : E;
    out[0][1] = B == F && par1 && (!CE || art1 || C == J || C == L) ? B : E;
    out[1][0] = D == H && par1 && (!EG || art1 || G == K || G == M) ? H : E;
    out[1][1] = F == H && par0 && (!EI || art0 || I == L || I == M) ? H : E;
}

static void ref_scale3x(const ref_around_t *n, bool sfx, uint32_t out[3][3])
{
    const uint32_t A = n->A, B = n->B, C = n->C, D = n->D, E = n->E, F = n->F, G = n->G, H = n->H, I = n->I;
    const uint32_t J = n->J, K = n->K, L = n->L, M = n->M;
    if (!sfx) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) out[r][c] = E;
        if (!(B != H && D != F)) return;
        const bool eqBD = B == D, eqBF = B == F, eqHD = H == D, eqHF = H == F;
        const bool neqEA = E != A, neqEC = E != C, neqEG = E != G, neqEI = E != I;
        out[0][0] = eqBD ? B : E;
        out[0][1] = (eqBD && neqEC) || (eqBF && neqEA) ? B : E;
        out[0][2] = eqBF ? B : E;
        out[1][0] = (eqBD && neqEG) || (eqHD && neqEA) ? D : E;
        out[1][2] = (eqBF && neqEI) || (eqHF && neqEC) ? F : E;
        out[2][0] = eqHD ? H : E;
        out[2][1] = (eqHD && neqEI) || (eqHF && neqEG) ? H : E;
        out[2][2] = eqHF ? H : E;
        return;
    }
    const bool AE = A == E, AJ = A == J, AK = A == K, BD = B == D, BF = B == F, CE = C == E, CJ = C == J, CL = C == L;
    const bool DH = D == H, EG = E == G, EI = E == I, FH = F == H, GK = G == K, GM = G == M, IL = I == L, IM = I == M;
    const bool par0 = !(BF || DH), par1 = !(BD || FH);
    const bool dia0 = AE || EI, dia1 = CE || EG;
    const bool art0 = BD && par0 && (!AE || dia1 || AJ || AK);
    const bool art2 = BF && par1 && (!CE || dia0 || CJ || CL);
    const bool art6 = DH && par1 && (!EG || dia0 || GK || GM);
    const bool art8 = FH && par0 && (!EI || dia1 || IL || IM);
    out[0][0] = art0 || (BD && ((CE && !CJ && !AE) || (EG && !AE && !GK))) ? B : E;
    out[0][2] = art2 || (BF && ((AE && !AJ && !CE) || (EI && !CE && !IL))) ? B : E;
    out[2][0] = art6 || (DH && ((AE && !AK && !EG) || (EI && !EG && !IM))) ? H : E;
    out[2][2] = art8 || (FH && ((CE && !CL && !EI) || (EG && !EI && !GM))) ? H : E;
    out[0][1] = (art0 && !CE) || (art2 && !AE) ? B : E;
    out[1][0] = (art0 && !EG) || (art6 && !AE) ? D : E;
    out[1][2] = (art8 && !CE) || (art2 && !EI) ? F : E;
    out[2][1] = (art8 && !EG) || (art6 && !EI) ? H : E;
    out[1][1] = E;
}

/* the picture made `level` (1 or 2) times as large by Scale2x, or as it is at 0, at a point anywhere */
static uint32_t ref_twice(const ref_picture_t *p, bool sfx, int level, int x, int y);

static void ref_around(const ref_picture_t *p, bool sfx, int level, int x, int y, ref_around_t *n)
{
#define REF_V(dx, dy) ref_twice(p, sfx, level, x + (dx), y + (dy))
    n->A = REF_V(-1, -1), n->B = REF_V(0, -1), n->C = REF_V(1, -1);
    n->D = REF_V(-1, 0), n->E = REF_V(0, 0), n->F = REF_V(1, 0);
    n->G = REF_V(-1, 1), n->H = REF_V(0, 1), n->I = REF_V(1, 1);
    n->J = REF_V(0, -2), n->K = REF_V(-2, 0), n->L = REF_V(2, 0), n->M = REF_V(0, 2);
#undef REF_V
}

static uint32_t ref_twice(const ref_picture_t *p, bool sfx, int level, int x, int y)
{
    if (!level) return ref_at(p, x, y);
    ref_around_t n;
    ref_around(p, sfx, level - 1, ref_floor_div(x, 2), ref_floor_div(y, 2), &n);
    uint32_t out[2][2];
    ref_scale2x(&n, sfx, out);
    return out[y - 2 * ref_floor_div(y, 2)][x - 2 * ref_floor_div(x, 2)];
}

/*
 * The point (x, y) of the picture made n times as large: blocks of n by n,
 * or smoothed at 2, 3 and 4 by REF_SMOOTH (Scale2x, Scale3x) or REF_SFX.
 * Any other scale is blocks whatever is asked.
 */
static uint32_t ref_larger(const ref_picture_t *p, int n, int smooth, int x, int y)
{
    if (smooth == REF_BLOCKS || (n != 2 && n != 3 && n != 4)) return ref_at(p, ref_floor_div(x, n), ref_floor_div(y, n));
    const bool sfx = smooth == REF_SFX;
    if (n == 2) return ref_twice(p, sfx, 1, x, y);
    if (n == 4) return ref_twice(p, sfx, 2, x, y);
    ref_around_t n3;
    ref_around(p, sfx, 0, ref_floor_div(x, 3), ref_floor_div(y, 3), &n3);
    uint32_t out[3][3];
    ref_scale3x(&n3, sfx, out);
    return out[y - 3 * ref_floor_div(y, 3)][x - 3 * ref_floor_div(x, 3)];
}
