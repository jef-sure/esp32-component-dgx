#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_screen_functions.h"
#include "drivers/vscreen.h"

/*
 * The outline of an ellipse, its arc and its sector. The outline is checked
 * against the rule it is drawn by, asked of every pixel apart; against the
 * curve itself, which may not be further than half a pixel from a pixel of
 * it; against the circle of the library and the walk of the textbooks. An
 * arc of a pixel and a sector are the pixels of the ellipse between two
 * rays. An arc by a pen is checked by how far a pixel of it lies from the
 * curve. And against waste: no pixel is sent twice.
 */

#define W 700
#define H 700
#define CX 340
#define CY 330

static uint8_t canvas[H][W], model[H][W], other[H][W];
static int     fills, empty_fills, pixels_set;
static long    written;

static void put_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    (void)scr;
    ++fills;
    if (w <= 0 || h <= 0) ++empty_fills;
    for (int j = y; j < y + h; ++j) {
        if (j < 0 || j >= H) continue;
        for (int i = x; i < x + w; ++i) {
            if (i >= 0 && i < W) canvas[j][i] = (uint8_t)color, ++written;
        }
    }
}

static void put_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    (void)scr;
    ++pixels_set;
    if (x >= 0 && x < W && y >= 0 && y < H) canvas[y][x] = (uint8_t)color, ++written;
}

static void dot(int x, int y)
{
    if (x >= 0 && x < W && y >= 0 && y < H) model[y][x] = 7;
}

/* a figure takes so many rows to each side of the middle one: only they are cleared, compared and counted */
static int rows = H;

static int first_row(void)
{
    return CY - rows < 0 ? 0 : CY - rows;
}

static size_t rows_size(void)
{
    const int last = CY + rows >= H ? H - 1 : CY + rows;
    return (size_t)(last - first_row() + 1) * W;
}

static void clear(uint8_t (*a)[W])
{
    memset(a[first_row()], 0, rows_size());
}

static void keep(uint8_t (*to)[W], uint8_t (*from)[W])
{
    memcpy(to[first_row()], from[first_row()], rows_size());
}

static bool equal(uint8_t (*a)[W], uint8_t (*b)[W])
{
    return memcmp(a[first_row()], b[first_row()], rows_size()) == 0;
}

static long count(uint8_t (*a)[W])
{
    long           n = 0;
    const uint8_t *p = a[first_row()];
    for (size_t i = rows_size(); i; --i) n += *p++ != 0;
    return n;
}

static void start(void)
{
    clear(canvas);
    clear(model);
    fills = empty_fills = pixels_set = 0;
    written = 0;
}

/* a new figure of this radius along y: what the one before it left is cleared first */
static void figure(int ry)
{
    start();
    clear(other);
    rows = ry + 16;
}

static bool same(const char *what, int rx, int ry, double from, double to)
{
    CHECK(empty_fills == 0);
    if (equal(canvas, model)) return true;
    fprintf(stderr, "%s of %d, %d from %g to %g differs\n", what, rx, ry, from, to);
    ++check_failures;
    return false;
}

/* the rule: the curve reaches the middle of the side of the pixel that faces the centre, across the row or across the column */
static bool in_ellipse(int dx, int dy, int rx, int ry)
{
    dx = abs(dx), dy = abs(dy);
    if (dx > rx || dy > ry) return false;
    if (rx == 0) return dx == 0;
    if (ry == 0) return dy == 0;
    if (dx == 0 || dy == 0) return true;
    const double a = (double)ry * ry, b = (double)rx * rx, c = a * b;
    return a * (dx - 0.5) * (dx - 0.5) + b * dy * dy < c || a * dx * dx + b * (dy - 0.5) * (dy - 0.5) < c;
}

/* of the outline: of the ellipse, with a pixel next to it in its row or its column that is not */
static bool on_outline(int dx, int dy, int rx, int ry)
{
    if (!in_ellipse(dx, dy, rx, ry)) return false;
    return !in_ellipse(dx + 1, dy, rx, ry) || !in_ellipse(dx - 1, dy, rx, ry) || !in_ellipse(dx, dy + 1, rx, ry) ||
           !in_ellipse(dx, dy - 1, rx, ry);
}

/*
 * Where a pixel is to two rays, angles in degrees: 1 between them, 0 not,
 * -1 too near a ray to tell by numbers that are not exact.
 */
static int in_wedge(int dx, int dy, double from, double to)
{
    if (to < from) {
        const double t = from;
        from = to, to = t;
    }
    if (to - from >= 360) return 1;
    if (dx == 0 && dy == 0) return 1;
    const double far = hypot(dx, dy), turn = 8 * atan(1.0);
    double       angle = atan2(-dy, dx) * 360 / turn;
    while (angle < from) angle += 360;
    while (angle - 360 >= from) angle -= 360;
    /* how far the pixel is from each ray, pixels */
    for (int i = 0; i < 2; ++i) {
        const double ray = (i ? to : from) * turn / 360;
        const double along = dx * cos(ray) - dy * sin(ray), across = dx * sin(ray) + dy * cos(ray);
        if (along > -1 && fabs(across) < 0.02 + far * 1.5 / 16384) return -1;
    }
    return angle <= to;
}

/* the same for rays that are multiples of an eighth of a turn, by whole numbers: a pixel on a ray is between them */
static bool in_wedge_eighths(int dx, int dy, int from, int to)
{
    static const int ray[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    if (to < from) {
        const int t = from;
        from = to, to = t;
    }
    if (to - from >= 8) return true;
    const int *a = ray[((from % 8) + 8) % 8], *b = ray[((to % 8) + 8) % 8];
    /* y goes down on the screen */
    const int  past_first = a[0] * -dy - a[1] * dx, before_second = b[1] * dx - b[0] * -dy;
    return to - from > 4 ? past_first >= 0 || before_second >= 0 : past_first >= 0 && before_second >= 0;
}

/* the midpoint ellipse as the textbooks give it, in whole numbers: every decision times four */
static void midpoint_ellipse(int x0, int y0, int rx, int ry)
{
    const int64_t a = (int64_t)ry * ry, b = (int64_t)rx * rx;
    int64_t       x = 0, y = ry, dx = 0, dy = 2 * b * y;
    int64_t       d = 4 * a - 4 * b * ry + b;
    while (dx < dy) {
        dot(x0 + x, y0 + y), dot(x0 - x, y0 + y), dot(x0 + x, y0 - y), dot(x0 - x, y0 - y);
        ++x, dx += 2 * a;
        if (d < 0) {
            d += 4 * (dx + a);
        } else {
            --y, dy -= 2 * b;
            d += 4 * (dx - dy + a);
        }
    }
    d = a * (2 * x + 1) * (2 * x + 1) + 4 * b * (y - 1) * (y - 1) - 4 * a * b;
    while (y >= 0) {
        dot(x0 + x, y0 + y), dot(x0 - x, y0 + y), dot(x0 + x, y0 - y), dot(x0 - x, y0 - y);
        --y, dy -= 2 * b;
        if (d > 0) {
            d += 4 * (b - dy);
        } else {
            ++x, dx += 2 * a;
            d += 4 * (dx - dy + b);
        }
    }
}

static int from_textbook, from_textbook_fat, of_textbook, of_textbook_fat;

static void outline(dgx_screen_t *scr, int rx, int ry)
{
    figure(ry);
    for (int dy = -ry - 2; dy <= ry + 2; ++dy) {
        for (int dx = -rx - 2; dx <= rx + 2; ++dx) {
            if (on_outline(dx, dy, rx, ry)) dot(CX + dx, CY + dy);
        }
    }
    dgx_draw_ellipse(scr, CX, CY, rx, ry, 7);
    same("ellipse", rx, ry, 0, 0);
    /* every pixel once */
    CHECK(written == count(model));
    /* the curve is within half a pixel of every pixel, along its row or along its column */
    if (rx && ry) {
        for (int dy = 0; dy <= ry; ++dy) {
            for (int dx = 0; dx <= rx; ++dx) {
                if (!canvas[CY + dy][CX + dx]) continue;
                const double x_there = rx * sqrt(1 - (double)dy * dy / ((double)ry * ry));
                const double y_there = ry * sqrt(1 - (double)dx * dx / ((double)rx * rx));
                CHECK(fabs(x_there - dx) <= 0.5 + 1e-9 || fabs(y_there - dy) <= 0.5 + 1e-9);
            }
        }
    }
    /* the circle of the library, and the walk of the textbooks where it is right */
    if (rx == ry) {
        keep(other, canvas);
        clear(canvas);
        dgx_draw_circle(scr, CX, CY, rx, 7);
        CHECK(equal(canvas, other));
    } else if (rx && ry) {
        clear(model);
        midpoint_ellipse(CX, CY, rx, ry);
        const bool fat = rx >= 4 && ry >= 4 && rx <= 70 && ry <= 70 && rx <= 8 * ry && ry <= 8 * rx;
        const bool as_walked = equal(model, canvas);
        ++of_textbook, from_textbook += !as_walked;
        if (fat) ++of_textbook_fat, from_textbook_fat += !as_walked;
    }
}

/* the ellipse filled, or its outline, between two rays that are multiples of an eighth of a turn */
static void eighths(dgx_screen_t *scr, int rx, int ry, int from, int to, bool solid)
{
    figure(ry);
    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dx = -rx; dx <= rx; ++dx) {
            if (from == to || !in_wedge_eighths(dx, dy, from, to)) continue;
            if (solid ? in_ellipse(dx, dy, rx, ry) : on_outline(dx, dy, rx, ry)) dot(CX + dx, CY + dy);
        }
    }
    if (solid) dgx_solid_sector(scr, CX, CY, rx, ry, from * 45.0f, to * 45.0f, 7);
    else dgx_draw_arc(scr, CX, CY, rx, ry, from * 45.0f, to * 45.0f, 1, 7);
    same(solid ? "sector" : "arc", rx, ry, from * 45.0, to * 45.0);
    CHECK(written == count(model));
    /* a sector goes by rows, no more pieces than two a row */
    if (solid) CHECK(fills + pixels_set <= 2 * (2 * ry + 1));
}

/* the same between any two rays: a pixel too near a ray to tell may be either */
static void between(dgx_screen_t *scr, int rx, int ry, double from, double to, bool solid)
{
    figure(ry);
    if (solid) dgx_solid_sector(scr, CX, CY, rx, ry, (float)from, (float)to, 7);
    else dgx_draw_arc(scr, CX, CY, rx, ry, (float)from, (float)to, 1, 7);
    int wrong = 0, near = 0;
    for (int dy = -ry - 2; dy <= ry + 2; ++dy) {
        for (int dx = -rx - 2; dx <= rx + 2; ++dx) {
            const bool of_figure = solid ? in_ellipse(dx, dy, rx, ry) : on_outline(dx, dy, rx, ry);
            const int  where = in_wedge(dx, dy, (float)from, (float)to);
            const bool drawn = canvas[CY + dy][CX + dx] != 0;
            if (where < 0 && of_figure) ++near;
            else wrong += drawn != (of_figure && where == 1);
        }
    }
    if (wrong) fprintf(stderr, "%s of %d, %d from %g to %g: %d pixels wrong\n", solid ? "sector" : "arc", rx, ry, from, to, wrong);
    CHECK(wrong == 0);
    /* those near a ray are few: a pixel or two a row at each ray */
    CHECK(near <= 8 * (rx + ry + 2));
    CHECK(written == count(canvas));
    CHECK(empty_fills == 0);
}

static double rnd(void)
{
    static uint32_t seed = 12345;
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / (double)(1 << 24);
}

/* points of the arc of the ellipse between two angles of the screen, a third of a pixel apart or closer */
static double (*arc_points)[2];
static int    arc_points_n;

static void arc_sample(int rx, int ry, double from, double to)
{
    const double turn = 8 * atan(1.0);
    if (to < from) {
        const double t = from;
        from = to, to = t;
    }
    if (to - from > 360) to = from + 360;
    /* a ray turned by an angle moves its point by the larger radius times the angle, and by their ratio more where the ellipse is thin */
    const double large = rx > ry ? rx : ry, small = rx > ry ? ry : rx;
    arc_points_n = 2 + (int)((to - from) * turn / 360 * large * (large / small) * 3);
    arc_points = realloc(arc_points, sizeof(*arc_points) * arc_points_n);
    for (int i = 0; i < arc_points_n; ++i) {
        const double angle = (from + (to - from) * i / (arc_points_n - 1)) * turn / 360;
        /* the place where the ray of the angle meets the curve */
        const double c = cos(angle), s = sin(angle), far = rx * (double)ry / hypot(ry * c, rx * s);
        arc_points[i][0] = CX + far * c, arc_points[i][1] = CY - far * s;
    }
}

/* marks in `model` the pixels within `far` of the arc */
static void arc_around(double far, double shift)
{
    clear(model);
    if (far < 0) return;
    for (int i = 0; i < arc_points_n; ++i) {
        const double px = arc_points[i][0] + shift, py = arc_points[i][1] + shift;
        for (int y = (int)floor(py - far); y <= (int)ceil(py + far); ++y) {
            for (int x = (int)floor(px - far); x <= (int)ceil(px + far); ++x) {
                if (y >= 0 && y < H && x >= 0 && x < W && hypot(x - px, y - py) <= far) model[y][x] = 1;
            }
        }
    }
}

/*
 * An arc by a pen: a pixel well within half the width of the pen from the
 * curve has all the ink, a pixel well out of it has none. The ends of the
 * arc are brought to whole pixels and its pieces are within half a pixel of
 * the curve, so "well" is a pixel and a half.
 */
static void pen_arc(dgx_screen_t *v, int rx, int ry, double from, double to, int pen, bool soft)
{
    rows = H;
    clear(model);
    rows = ry + pen + 8;
    dgx_fill_rectangle(v, 0, 0, W, H, 0);
    dgx_draw_arc(v, CX, CY, rx, ry, (float)from, (float)to, soft ? DGX_PEN_SOFT | pen : pen, 200);
    arc_sample(rx, ry, from, to);
    /* a pen of an even width runs half a pixel to the side: between pixels when soft, to the right of its way when hard */
    const double shift = soft && !(pen & 1) ? 0.5 : 0;
    const double slack = soft || (pen & 1) ? 1.5 : 2.0;
    int          missing = 0, stray = 0, inked = 0, half = 0;
    arc_around(pen * 0.5 - slack, shift);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint32_t ink = dgx_get_pixel(v, x, y);
            inked += ink != 0;
            half += ink != 0 && ink != 200;
            if (model[y][x] && ink != 200) ++missing;
        }
    }
    arc_around(pen * 0.5 + slack, shift);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!model[y][x] && dgx_get_pixel(v, x, y) != 0) ++stray;
        }
    }
    if (missing || stray) {
        fprintf(stderr, "arc of %d, %d from %g to %g by a %s pen of %d: %d pixels without ink, %d with ink out of it\n", rx, ry, from, to,
                soft ? "soft" : "hard", pen, missing, stray);
        ++check_failures;
    }
    CHECK(inked > 0);
    /* a soft edge has shares of the ink, a hard one has none */
    if (soft) CHECK(half > 0);
    else CHECK(half == 0);
}

/*
 * Where a soft line runs: the middle of its ink across the line, found every half a degree, against the circle
 * it is drawn along. The pieces of a soft arc are within an eighth of a pixel of it; the middle points of its
 * curves are not whole pixels, and made whole they would move the line by a third of a pixel.
 */
static double soft_middle(dgx_screen_t *v, int r, int pen)
{
    dgx_fill_rectangle(v, 0, 0, W, H, 0);
    dgx_draw_arc(v, CX, CY, r, r, 0, 360, DGX_PEN_SOFT | pen, 255);
    /* a pen of an even width runs half a pixel to the right and down */
    const double shift = pen & 1 ? 0 : 0.5, turn = 8 * atan(1.0);
    double       worst = 0;
    for (int i = 0; i < 720; ++i) {
        const double c = cos(i * turn / 720), s = -sin(i * turn / 720);
        double       ink = 0, moment = 0;
        for (double d = r - pen; d <= r + pen; d += 0.1) {
            /* the ink at a point: of the four pixels around it, by how near each is */
            const double x = CX + shift + d * c, y = CY + shift + d * s;
            const int    x0 = (int)floor(x), y0 = (int)floor(y);
            const double fx = x - x0, fy = y - y0;
            const double p = dgx_get_pixel(v, x0, y0) * (1 - fx) * (1 - fy) + dgx_get_pixel(v, x0 + 1, y0) * fx * (1 - fy) +
                             dgx_get_pixel(v, x0, y0 + 1) * (1 - fx) * fy + dgx_get_pixel(v, x0 + 1, y0 + 1) * fx * fy;
            ink += p, moment += p * d;
        }
        if (fabs(moment / ink - r) > worst) worst = fabs(moment / ink - r);
    }
    return worst;
}

int main(void)
{
    /* a screen that only counts what it is asked for */
    static dgx_screen_t screen;
    dgx_screen_t       *scr = &screen;
    scr->width              = W;
    scr->height             = H;
    scr->color_bits         = 8;
    scr->screen_subtype     = DgxVirtualScreen; /* no bus behind it: only the drawing functions are taken */
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->fill_rectangle = put_rectangle;
    scr->set_pixel      = put_pixel;

    for (int rx = 0; rx <= 70; ++rx) {
        for (int ry = 0; ry <= 70; ++ry) outline(scr, rx, ry);
    }
    for (int r = 71; r <= 320; r += 3) outline(scr, r, r);
    /* thin ones and large ones */
    for (int r = 1; r <= 6; ++r) {
        for (int far = 80; far <= 320; far += 37) outline(scr, r, far), outline(scr, far, r);
    }
    outline(scr, 300, 177), outline(scr, 123, 311), outline(scr, 319, 318);
    printf("of %d ellipses %d differ from the textbook walk; of %d with radii from 4 to 70 and no thinner than 8 to 1, %d\n", of_textbook,
           from_textbook, of_textbook_fat, from_textbook_fat);

    static const int radii[][2] = {{0, 0}, {1, 1}, {5, 5}, {40, 40}, {97, 97}, {60, 25}, {25, 60}, {150, 3}, {3, 150}, {0, 30}, {30, 0}, {211, 140}};
    for (unsigned i = 0; i < sizeof(radii) / sizeof(radii[0]); ++i) {
        for (int from = -9; from <= 9; ++from) {
            for (int to = from - 9; to <= from + 9; ++to) {
                eighths(scr, radii[i][0], radii[i][1], from, to, true);
                eighths(scr, radii[i][0], radii[i][1], from, to, false);
            }
        }
    }
    /* a whole turn is the ellipse filled, and with equal radii the solid circle of the library */
    for (int r = 0; r <= 150; ++r) {
        figure(r);
        dgx_solid_sector(scr, CX, CY, r, r, 30, 390, 7);
        keep(other, canvas);
        const int pieces = fills + pixels_set;
        clear(canvas);
        dgx_solid_circle(scr, CX, CY, r, 7);
        CHECK(equal(canvas, other));
        /* rows that are equal go as one rectangle: no more pieces than rows */
        CHECK(pieces <= 2 * r + 1);
        start();
        dgx_draw_arc(scr, CX, CY, r, r, -10, 350, 1, 7);
        keep(other, canvas);
        clear(canvas);
        dgx_draw_circle(scr, CX, CY, r, 7);
        CHECK(equal(canvas, other));
    }
    for (int i = 0; i < 3000; ++i) {
        const int    rx = (int)(rnd() * 200), ry = (int)(rnd() * 200);
        const double from = rnd() * 1000 - 500, to = from + rnd() * 800 - 400;
        between(scr, rx, ry, from, to, i & 1);
    }
    /* two sectors that share their rays are the ellipse, and have only the rays in common */
    for (int i = 0; i < 300; ++i) {
        const int    rx = 1 + (int)(rnd() * 150), ry = 1 + (int)(rnd() * 150);
        const float  from = (float)(rnd() * 360), to = from + (float)(1 + rnd() * 358);
        figure(ry);
        dgx_solid_sector(scr, CX, CY, rx, ry, from, to, 7);
        keep(other, canvas);
        clear(canvas);
        dgx_solid_sector(scr, CX, CY, rx, ry, to, from + 360, 7);
        long common = 0;
        for (int dy = -ry - 1; dy <= ry + 1; ++dy) {
            for (int dx = -rx - 1; dx <= rx + 1; ++dx) {
                const bool one = other[CY + dy][CX + dx] != 0, two = canvas[CY + dy][CX + dx] != 0;
                CHECK((one || two) == in_ellipse(dx, dy, rx, ry));
                common += one && two;
            }
        }
        CHECK(common >= 1 && common <= 2 * (rx + ry) + 2);
    }
    /* nothing between the rays, no pen, no figure: nothing drawn; cut by the edges of the screen and off it: no harm */
    figure(H);
    dgx_solid_sector(scr, CX, CY, 50, 40, 77, 77, 7);
    dgx_draw_arc(scr, CX, CY, 50, 40, 77, 77, 1, 7);
    dgx_draw_arc(scr, CX, CY, 50, 40, 10, 77, 0, 7);
    dgx_draw_arc(scr, CX, CY, 50, 40, 10, 77, DGX_PEN_SOFT, 7);
    dgx_draw_arc(scr, CX, CY, 50, 40, 10, NAN, 3, 7);
    dgx_draw_ellipse(scr, CX, CY, -1, 40, 7);
    dgx_solid_sector(scr, CX, CY, 50, -3, 0, 90, 7);
    dgx_draw_ellipse(scr, -500, CY, 50, 40, 7);
    dgx_solid_sector(scr, CX, H + 300, 50, 40, 0, 360, 7);
    CHECK(written == 0 && fills == 0 && pixels_set == 0);
    for (int r = 5; r <= 90; r += 17) {
        dgx_draw_ellipse(scr, 3, 5, r, r / 2 + 1, 7);
        dgx_draw_ellipse(scr, W - 2, H - 4, r / 2 + 1, r, 7);
        dgx_solid_sector(scr, -r / 2, H / 2, r, r + 9, 20, 300, 7);
        dgx_solid_sector(scr, W / 2, H + r / 3, r + 9, r, -100, 60, 7);
        dgx_draw_arc(scr, W - 3, 2, r, r, 0, 360, 1, 7);
        dgx_draw_arc(scr, 1, H - 1, r, r, 0, 360, 4, 7);
    }
    CHECK(empty_fills == 0);

    /* by a pen: soft where a pixel is an amount of ink, hard on a screen of colors, whatever is asked for */
    dgx_screen_t *ink = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_screen_t *colors = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    static const int pens[] = {3, 4, 5, 8, 11};
    for (unsigned p = 0; p < sizeof(pens) / sizeof(pens[0]); ++p) {
        for (int i = 0; i < 40; ++i) {
            const int    rx = 12 + (int)(rnd() * 180), ry = i & 1 ? rx : 12 + (int)(rnd() * 180);
            const double from = rnd() * 720 - 360, to = from + (i % 5 ? rnd() * 420 - 30 : 360);
            pen_arc(ink, rx, ry, from, to, pens[p], true);
            pen_arc(ink, rx, ry, from, to, pens[p], false);
            pen_arc(colors, rx, ry, from, to, pens[p], false);
        }
    }
    double off = 0;
    for (int pen = 3; pen <= 6; ++pen) {
        for (int r = 8; r <= 160; r = r < 40 ? r + 4 : r * 2) {
            const double here = soft_middle(ink, r, pen);
            if (here > off) off = here;
        }
    }
    printf("the middle of a soft arc is off its circle by %.3f of a pixel at most\n", off);
    CHECK(off <= 0.25);
    /* the sign of Wi-Fi: arcs of a quarter of a turn over a dot, the same to the left and to the right */
    rows = H;
    dgx_fill_rectangle(ink, 0, 0, W, H, 0);
    for (int r = 12; r <= 36; r += 12) dgx_draw_arc(ink, CX, CY, r, r, 45, 135, DGX_PEN_SOFT | 5, 255);
    int skew = 0, most = 0;
    for (int y = CY - 50; y <= CY + 10; ++y) {
        for (int dx = 1; dx <= 50; ++dx) {
            const int d = abs((int)dgx_get_pixel(ink, CX + dx, y) - (int)dgx_get_pixel(ink, CX - dx, y));
            skew += d != 0;
            if (d > most) most = d;
        }
    }
    printf("the sign of Wi-Fi by a soft pen of 5: %d pixels differ from their mirror, by %d of 255 at most\n", skew, most);
    CHECK(most <= 16);
    /* a soft pen of 1 is a pen, a hard pen of 1 asked to be soft on a screen of colors is the outline */
    dgx_fill_rectangle(colors, 0, 0, W, H, 0);
    dgx_draw_arc(colors, CX, CY, 80, 50, 0, 360, DGX_PEN_SOFT | 1, 9);
    int differ = 0;
    for (int dy = -52; dy <= 52; ++dy) {
        for (int dx = -82; dx <= 82; ++dx) differ += (dgx_get_pixel(colors, CX + dx, CY + dy) != 0) != on_outline(dx, dy, 80, 50);
    }
    CHECK(differ == 0);
    pen_arc(ink, 80, 50, 0, 360, 1, true);
    /*
     * An angle a hair below 0, as a float stepped down to 0 or atan2f() of a point just under the axis
     * gives: the ray that goes right. The arc from it is the arc from 0, not the whole ellipse and a
     * quarter more, and the arc to it is the arc to 0, not nothing.
     */
    dgx_screen_t     *hair_screen = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    static const float hairs[]    = {-1e-5f, -5.7e-6f, -1e-9f};
    for (unsigned i = 0; i < sizeof(hairs) / sizeof(hairs[0]); ++i) {
        for (int soft = 0; soft < 2; ++soft) {
            const int pen = soft ? DGX_PEN_SOFT | 3 : 3;
            dgx_fill_rectangle(ink, 0, 0, W, H, 0);
            dgx_fill_rectangle(hair_screen, 0, 0, W, H, 0);
            dgx_draw_arc(ink, CX, CY, 60, 40, 0, 90, pen, 255);
            dgx_draw_arc(hair_screen, CX, CY, 60, 40, hairs[i], 90, pen, 255);
            CHECK(dgx_get_pixel(ink, CX + 60, CY - 1) == 255);
            CHECK(memcmp(((dgx_vscreen_t *)ink)->v_array, ((dgx_vscreen_t *)hair_screen)->v_array, W * H) == 0);
            dgx_fill_rectangle(ink, 0, 0, W, H, 0);
            dgx_fill_rectangle(hair_screen, 0, 0, W, H, 0);
            dgx_draw_arc(ink, CX, CY, 60, 40, -90, 0, pen, 255);
            dgx_draw_arc(hair_screen, CX, CY, 60, 40, -90, hairs[i], pen, 255);
            CHECK(dgx_get_pixel(ink, CX + 60, CY + 1) == 255);
            CHECK(memcmp(((dgx_vscreen_t *)ink)->v_array, ((dgx_vscreen_t *)hair_screen)->v_array, W * H) == 0);
        }
    }
    dgx_screen_destroy(&hair_screen);
    free(arc_points);
    dgx_screen_destroy(&ink);
    dgx_screen_destroy(&colors);
    CHECK_DONE();
}
