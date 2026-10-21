/*
 * dgx_draw_polygon4_solid() with convex and concave quadrilaterals: every
 * pixel whose centre lies inside, more than half a pixel from the outline,
 * is filled, and no pixel further than a pixel from the quadrilateral is.
 * A concave one with two tips side by side is not joined across the gap.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "check.h"
#include "dgx_draw.h"
#include "drivers/vscreen.h"

#define W 64
#define H 64

static uint32_t seed = 12345;

static int rnd(int lo, int hi)
{
    seed = seed * 1103515245u + 12345u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

static double cross(double ax, double ay, double bx, double by)
{
    return ax * by - ay * bx;
}

static bool inside(const int vx[4], const int vy[4], double px, double py)
{
    bool in = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        if ((vy[i] > py) != (vy[j] > py) && px < vx[j] + (vx[i] - vx[j]) * (py - vy[j]) / (double)(vy[i] - vy[j])) in = !in;
    }
    return in;
}

static double distance_to_outline(const int vx[4], const int vy[4], double px, double py)
{
    double best = 1e9;
    for (int i = 0; i < 4; ++i) {
        const int    j = (i + 1) & 3;
        const double ex = vx[j] - vx[i], ey = vy[j] - vy[i], len2 = ex * ex + ey * ey;
        double       t = len2 > 0 ? ((px - vx[i]) * ex + (py - vy[i]) * ey) / len2 : 0;
        t              = t < 0 ? 0 : t > 1 ? 1 : t;
        const double dx = px - (vx[i] + t * ex), dy = py - (vy[i] + t * ey);
        const double d  = sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    return best;
}

/* no two edges that do not meet at a vertex cross */
static bool simple(const int vx[4], const int vy[4])
{
    for (int i = 0; i < 2; ++i) {
        const int a0 = i, a1 = i + 1, b0 = i + 2, b1 = (i + 3) & 3;
        const double d1 = cross(vx[a1] - vx[a0], vy[a1] - vy[a0], vx[b0] - vx[a0], vy[b0] - vy[a0]);
        const double d2 = cross(vx[a1] - vx[a0], vy[a1] - vy[a0], vx[b1] - vx[a0], vy[b1] - vy[a0]);
        const double d3 = cross(vx[b1] - vx[b0], vy[b1] - vy[b0], vx[a0] - vx[b0], vy[a0] - vy[b0]);
        const double d4 = cross(vx[b1] - vx[b0], vy[b1] - vy[b0], vx[a1] - vx[b0], vy[a1] - vy[b0]);
        if (d1 * d2 <= 0 && d3 * d4 <= 0) return false;
    }
    return true;
}

static int check_quad(dgx_screen_t *scr, const int vx[4], const int vy[4])
{
    dgx_fill_rectangle(scr, 0, 0, W, H, 0);
    dgx_draw_polygon4_solid(scr, vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], vx[3], vy[3], 0xffff);
    int wrong = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool   set = dgx_get_pixel(scr, x, y) != 0;
            const double d   = distance_to_outline(vx, vy, x, y);
            const bool   in  = inside(vx, vy, x, y);
            if (!set && in && d > 0.5) ++wrong;
            if (set && !in && d > 1.0) ++wrong;
        }
    }
    return wrong;
}

int main(void)
{
    dgx_screen_t *scr = dgx_vscreen_init(W, H, 16, DgxScreenRGB);

    /* a chevron: two tips at the bottom on either side of the notch, which stays empty */
    const int cx[4] = {30, 50, 30, 10}, cy[4] = {5, 50, 30, 50};
    CHECK(check_quad(scr, cx, cy) == 0);
    CHECK(dgx_get_pixel(scr, 10, 50) != 0 && dgx_get_pixel(scr, 50, 50) != 0);
    for (int y = 34; y <= 50; ++y) CHECK(dgx_get_pixel(scr, 30, y) == 0);

    /* convex and concave quadrilaterals at random, either way round */
    int convex = 0, concave = 0;
    for (int round = 0; round < 4000; ++round) {
        int vx[4], vy[4];
        for (int i = 0; i < 4; ++i) vx[i] = rnd(2, W - 3), vy[i] = rnd(2, H - 3);
        if (!simple(vx, vy)) continue;
        int positive = 0, negative = 0;
        for (int i = 0; i < 4; ++i) {
            const int    p = (i + 3) & 3, n = (i + 1) & 3;
            const double c = cross(vx[i] - vx[p], vy[i] - vy[p], vx[n] - vx[i], vy[n] - vy[i]);
            positive += c > 0, negative += c < 0;
        }
        if (positive + negative < 4) continue;
        const int wrong = check_quad(scr, vx, vy);
        if (wrong) {
            fprintf(stderr, "quad (%d,%d) (%d,%d) (%d,%d) (%d,%d): %d pixels wrong\n", vx[0], vy[0], vx[1], vy[1], vx[2], vy[2], vx[3], vy[3], wrong);
            ++check_failures;
        }
        if (positive == 4 || negative == 4) ++convex;
        else ++concave;
    }
    CHECK(convex > 300 && concave > 300);
    dgx_screen_destroy(&scr);
    CHECK_DONE();
}
