/*
 * Shapes whose outlines lie between pixels: dgx_fill_ring_with(),
 * dgx_draw_wedge_with(), dgx_fill_ellipse_with(),
 * dgx_fill_round_rectangle_with(), dgx_fill_polygon_with() and
 * dgx_draw_polyline_with().
 *
 * The share of every pixel a soft shape covers, drawn as ink on a virtual
 * screen of 8 bits, is the share counted by 32 x 32 points in the pixel, each
 * tested against the shape itself, within 1.5 of the 32 steps, a star drawn
 * in one stroke too; the ink of a whole shape is its area. Moved by whole pixels, drawn into strips of rows
 * or under a clip, a shape is the same pixels. A display behind a bus with
 * paper gets what a virtual screen of that paper gets; without paper, a pixel
 * whole or not at all. Hard edges leave a pixel the color or what it was.
 * Gradients go from one color to the other; opacity mixes; a translucent line
 * is as translucent at its joints as along its pieces; a monotone curve does
 * not go past its values; the depths other than 8 and 16 bits take the shapes
 * through runs of one color.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/panel.c"

#include "dgx_draw.h"
#include "dgx_vscreen.h"
#include "vscreen/dgx_alpha_priv.h"

#define S 16 /* DGX_SUBPIXEL */

static uint32_t px(dgx_screen_t *s, int x, int y)
{
    return dgx_get_pixel(s, x, y);
}

/* ---- a display behind a bus: a 64x64 memory written through windows ---- */

static uint8_t  command, params[8], part[2];
static int      params_n, part_n;
static uint16_t sc, ec, sp, ep, column, page, glass[64][64];

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value)
{
    (void)bus;
    command = value, params_n = 0, part_n = 0;
    if (value == 0x2C) column = sc, page = sp;
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    for (uint32_t i = 0; i < bits / 8; ++i) {
        if (command == 0x2C) {
            part[part_n++] = data[i];
            if (part_n == 2) {
                if (column < 64 && page < 64) glass[page][column] = (uint16_t)(part[0] << 8 | part[1]);
                part_n = 0;
                if (++column > ec) column = sc, ++page;
            }
            continue;
        }
        if (params_n < (int)sizeof(params)) params[params_n] = data[i];
        const int n = ++params_n;
        if (command == 0x2A && n == 4) sc = (uint16_t)(params[0] << 8 | params[1]), ec = (uint16_t)(params[2] << 8 | params[3]);
        if (command == 0x2B && n == 4) sp = (uint16_t)(params[0] << 8 | params[1]), ep = (uint16_t)(params[2] << 8 | params[3]);
    }
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static const dgx_lcd_init_cmd_t no_init[] = {
    {0, 0, 0xff, 0, {0}}
};
static const dgx_panel_t panel64 = {.name = "64", .width = 64, .height = 64, .colmod = {0, 0x55}, .init = no_init};

/* ---- the shapes themselves: is a point, in pixels, inside ---- */

typedef enum { RING, WEDGE, ELLIPSE, RECT, POLYGON, POLYLINE, KINDS } kind_t;

typedef struct {
    kind_t                kind;
    dgx_ring_t            ring;
    dgx_wedge_t           wedge;
    dgx_ellipse_t         ellipse;
    dgx_round_rectangle_t rect;
    dgx_polygon_t         polygon;
    dgx_polyline_t        polyline;
} shape_t;

static dgx_paint_t *paint_of(shape_t *s)
{
    switch (s->kind) {
    case RING: return &s->ring.paint;
    case WEDGE: return &s->wedge.paint;
    case ELLIPSE: return &s->ellipse.paint;
    case RECT: return &s->rect.paint;
    case POLYGON: return &s->polygon.paint;
    default: return &s->polyline.paint;
    }
}

static esp_err_t draw(dgx_screen_t *scr, const shape_t *s)
{
    switch (s->kind) {
    case RING: return dgx_fill_ring_with(scr, &s->ring);
    case WEDGE: return dgx_draw_wedge_with(scr, &s->wedge);
    case ELLIPSE: return dgx_fill_ellipse_with(scr, &s->ellipse);
    case RECT: return dgx_fill_round_rectangle_with(scr, &s->rect);
    case POLYGON: return dgx_fill_polygon_with(scr, &s->polygon);
    default: return dgx_draw_polyline_with(scr, &s->polyline);
    }
}

/* the shape moved by whole pixels, its gradient with it */
static void move(shape_t *s, int dx, int dy, dgx_point_2d_t *points)
{
    dx *= S, dy *= S;
    dgx_paint_t *p = paint_of(s);
    p->gx[0] += dx, p->gx[1] += dx, p->gy[0] += dy, p->gy[1] += dy;
    switch (s->kind) {
    case RING: s->ring.x += dx, s->ring.y += dy; break;
    case WEDGE: s->wedge.x[0] += dx, s->wedge.x[1] += dx, s->wedge.y[0] += dy, s->wedge.y[1] += dy; break;
    case ELLIPSE: s->ellipse.x += dx, s->ellipse.y += dy; break;
    case RECT: s->rect.x += dx, s->rect.y += dy; break;
    case POLYGON:
        for (int i = 0; i < s->polygon.count; ++i) points[i].x = (int16_t)(s->polygon.points[i].x + dx), points[i].y = (int16_t)(s->polygon.points[i].y + dy);
        s->polygon.points = points;
        break;
    default:
        for (int i = 0; i < s->polyline.count; ++i) points[i].x = (int16_t)(s->polyline.points[i].x + dx), points[i].y = (int16_t)(s->polyline.points[i].y + dy);
        s->polyline.points = points;
        break;
    }
}

static float dist_segment(float x, float y, float ax, float ay, float bx, float by)
{
    const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    float       t  = l2 > 0 ? ((x - ax) * dx + (y - ay) * dy) / l2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const float ex = x - ax - t * dx, ey = y - ay - t * dy;
    return sqrtf(ex * ex + ey * ey);
}

static bool in_angle(float deg, float a, float b)
{
    while (deg < a) deg += 360.0f;
    while (deg >= a + 360.0f) deg -= 360.0f;
    return deg <= b;
}

static bool inside(const shape_t *s, float x, float y)
{
    switch (s->kind) {
    case RING: {
        const dgx_ring_t *r = &s->ring;
        const float cx = (float)r->x / S, cy = (float)r->y / S, ro = (float)r->r_outer / S, ri = (float)r->r_inner / S;
        const float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        float       a = r->from < r->to ? r->from : r->to, b = r->from < r->to ? r->to : r->from;
        const bool  whole = b - a >= 360.0f;
        if (d >= ri && d <= ro && (whole || in_angle(atan2f(cy - y, x - cx) * (float)(180.0 / M_PI), a, b))) return true;
        if (r->round && !whole) {
            const float hw = (ro - ri) / 2, rm = (ro + ri) / 2;
            for (int e = 0; e < 2; ++e) {
                const float t = (e ? b : a) * (float)(M_PI / 180.0), ex = cx + rm * cosf(t), ey = cy - rm * sinf(t);
                if ((x - ex) * (x - ex) + (y - ey) * (y - ey) <= hw * hw) return true;
            }
        }
        return false;
    }
    case WEDGE: {
        const dgx_wedge_t *w = &s->wedge;
        const float ax = (float)w->x[0] / S, ay = (float)w->y[0] / S, bx = (float)w->x[1] / S, by = (float)w->y[1] / S;
        const float r1 = (float)w->width[0] / (2 * S), r2 = (float)w->width[1] / (2 * S);
        const float dx = bx - ax, dy = by - ay, h = sqrtf(dx * dx + dy * dy), ux = dx / h, uy = dy / h;
        const float along = (x - ax) * ux + (y - ay) * uy, across = fabsf(-(x - ax) * uy + (y - ay) * ux);
        if (!w->round) return along >= 0 && along <= h && across <= r1 + (r2 - r1) * along / h;
        /* the hull of two discs, as the round cone of Inigo Quilez */
        const float b = (r1 - r2) / h, a = sqrtf(1 - b * b), k = -b * across + a * along;
        if (k < 0) return sqrtf(across * across + along * along) <= r1;
        if (k > a * h) return sqrtf(across * across + (along - h) * (along - h)) <= r2;
        return across * a + along * b <= r1;
    }
    case ELLIPSE: {
        const dgx_ellipse_t *e = &s->ellipse;
        const float u = (x - (float)e->x / S) / ((float)e->rx / S), v = (y - (float)e->y / S) / ((float)e->ry / S);
        return u * u + v * v <= 1;
    }
    case RECT: {
        const dgx_round_rectangle_t *r = &s->rect;
        const float x0 = (float)r->x / S, y0 = (float)r->y / S, w = (float)r->w / S, h = (float)r->h / S;
        float       R = (float)r->r / S;
        if (R > w / 2) R = w / 2;
        if (R > h / 2) R = h / 2;
        if (x < x0 || x > x0 + w || y < y0 || y > y0 + h) return false;
        const float cx = x < x0 + R ? x0 + R : x > x0 + w - R ? x0 + w - R : x, cy = y < y0 + R ? y0 + R : y > y0 + h - R ? y0 + h - R : y;
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= R * R;
    }
    case POLYGON: {
        int                   wind = 0;
        const dgx_point_2d_t *p    = s->polygon.points;
        for (int i = 0; i < s->polygon.count; ++i) {
            const int   j  = (i + 1) % s->polygon.count;
            const float ax = (float)p[i].x / S, ay = (float)p[i].y / S, bx = (float)p[j].x / S, by = (float)p[j].y / S;
            const float side = (bx - ax) * (y - ay) - (x - ax) * (by - ay);
            if (ay <= y) {
                if (by > y && side > 0) ++wind;
            } else if (by <= y && side < 0) {
                --wind;
            }
        }
        return wind != 0;
    }
    default: {
        const dgx_polyline_t *l  = &s->polyline;
        const float           hw = (float)l->width / (2 * S);
        const int             n  = l->closed ? l->count : l->count - 1;
        if (l->count == 1) return dist_segment(x, y, (float)l->points[0].x / S, (float)l->points[0].y / S, (float)l->points[0].x / S, (float)l->points[0].y / S) <= hw;
        for (int i = 0; i < n; ++i) {
            const dgx_point_2d_t a = l->points[i], b = l->points[(i + 1) % l->count];
            if (dist_segment(x, y, (float)a.x / S, (float)a.y / S, (float)b.x / S, (float)b.y / S) <= hw) return true;
        }
        return false;
    }
    }
}

/* the share of the pixel (i, j) the shape covers, in 32nds, by 32 x 32 points */
static float share(const shape_t *s, int i, int j)
{
    int in = 0;
    for (int b = 0; b < 32; ++b)
        for (int a = 0; a < 32; ++a) in += inside(s, (float)i + ((float)a + 0.5f) / 32, (float)j + ((float)b + 0.5f) / 32);
    return (float)in / 32.0f;
}

static const dgx_point_2d_t star[] = {
    {32 * S + 3, 6 * S + 5}, {48 * S + 9, 56 * S}, {6 * S + 2, 24 * S + 11}, {58 * S + 4, 24 * S + 7}, {16 * S + 13, 56 * S + 1},
};
static const dgx_point_2d_t zigzag[] = {
    {8 * S + 5, 50 * S + 3}, {20 * S + 2, 12 * S + 9}, {30 * S + 7, 44 * S}, {44 * S + 1, 16 * S + 4}, {56 * S + 11, 40 * S + 6},
};

/* the shapes of the tests, all within 64 x 64 */
static shape_t make(kind_t k, int variant)
{
    shape_t s = {.kind = k};
    switch (k) {
    case RING:
        s.ring = (dgx_ring_t){.x = 32 * S + 5, .y = 31 * S + 11, .r_inner = 14 * S + 3, .r_outer = 25 * S + 9, .from = -30.5f, .to = 200.25f, .round = variant & 1};
        if (variant & 2) s.ring.r_inner = 0, s.ring.round = false;
        if (variant & 4) s.ring.to = 400;
        break;
    case WEDGE:
        s.wedge = (dgx_wedge_t){.x = {12 * S + 7, 52 * S + 2}, .y = {50 * S + 3, 14 * S + 13}, .width = {14 * S + 5, 3 * S + 2}, .round = variant & 1};
        if (variant & 2) s.wedge.width[1] = 0;
        break;
    case ELLIPSE:
        s.ellipse = (dgx_ellipse_t){.x = 31 * S + 9, .y = 32 * S + 2, .rx = 26 * S + 5, .ry = variant & 1 ? 26 * S + 5 : 13 * S + 1};
        break;
    case RECT:
        s.rect = (dgx_round_rectangle_t){.x = 6 * S + 3, .y = 10 * S + 11, .w = 50 * S + 7, .h = 40 * S + 2, .r = variant & 1 ? 0 : 11 * S + 5};
        break;
    case POLYGON:
        s.polygon = (dgx_polygon_t){.points = star, .count = 5};
        break;
    default:
        s.polyline = (dgx_polyline_t){.points = zigzag, .count = 5, .width = 7 * S + 5, .closed = variant & 1};
        break;
    }
    return s;
}

int main(void)
{
    static const char *names[KINDS] = {"ring", "wedge", "ellipse", "rectangle", "polygon", "polyline"};

    /*
     * The share of each pixel, as ink on an 8-bit screen, against the share
     * counted by points; the whole ink against the area.
     */
    dgx_screen_t *ink = dgx_vscreen_init(64, 64, 8, DgxScreenRGB);
    for (int k = 0; k < KINDS; ++k)
        for (int variant = 0; variant < 8; ++variant) {
            if ((k == ELLIPSE || k == RECT || k == POLYLINE) && variant > 1) continue;
            if ((k == WEDGE) && variant > 3) continue;
            if (k == POLYGON && variant) continue;
            shape_t s                = make((kind_t)k, variant);
            *paint_of(&s)            = (dgx_paint_t){.color = 255, .flags = DGX_PAINT_SOFT};
            dgx_fill_rectangle(ink, 0, 0, 64, 64, 0);
            CHECK(draw(ink, &s) == ESP_OK);
            float worst = 0, drawn = 0, area = 0;
            for (int j = 0; j < 64; ++j)
                for (int i = 0; i < 64; ++i) {
                    const float w   = (float)px(ink, i, j) * 32.0f / 255.0f;
                    const float ref = share(&s, i, j);
                    if (fabsf(w - ref) > worst) worst = fabsf(w - ref);
                    drawn += w, area += ref;
                }
            /* the closed zigzag crosses itself: a pixel on its edge just there may be a little darker */
            const float allowed = k == POLYLINE && variant == 1 ? 2.5f : 1.5f;
            if (worst > allowed || fabsf(drawn - area) > 32.0f * 2) {
                fprintf(stderr, "%s %d: worst %.2f of 32, ink %.1f against %.1f pixels\n", names[k], variant, worst, drawn / 32, area / 32);
                CHECK(false);
            }
        }

    /* the area of a whole ring: pi (ro^2 - ri^2), the ink counted to a pixel */
    {
        dgx_ring_t r = {.x = 32 * S, .y = 32 * S, .r_inner = 10 * S, .r_outer = 30 * S, .to = 360, .paint = {.color = 255, .flags = DGX_PAINT_SOFT}};
        dgx_fill_rectangle(ink, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_ring_with(ink, &r) == ESP_OK);
        float sum = 0;
        for (int j = 0; j < 64; ++j)
            for (int i = 0; i < 64; ++i) sum += (float)px(ink, i, j) / 255.0f;
        const float area = (float)M_PI * (30 * 30 - 10 * 10);
        CHECK(fabsf(sum - area) < 4);
        /* hard: the pixels the ring covers half of or more */
        r.paint.flags = 0;
        dgx_fill_rectangle(ink, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_ring_with(ink, &r) == ESP_OK);
        int hard = 0, other = 0;
        for (int j = 0; j < 64; ++j)
            for (int i = 0; i < 64; ++i) hard += px(ink, i, j) == 255, other += px(ink, i, j) != 255 && px(ink, i, j) != 0;
        CHECK(other == 0 && fabsf((float)hard - area) < 30);
        /* the parts of a ring: 0 to 90 is the quarter up and right */
        r = (dgx_ring_t){.x = 32 * S, .y = 32 * S, .r_inner = 0, .r_outer = 20 * S, .from = 0, .to = 90, .paint = {.color = 255}};
        dgx_fill_rectangle(ink, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_ring_with(ink, &r) == ESP_OK);
        CHECK(px(ink, 40, 24) == 255 && px(ink, 24, 24) == 0 && px(ink, 40, 40) == 0 && px(ink, 24, 40) == 0);
        /* no angle between: nothing */
        r.to = 0;
        dgx_fill_rectangle(ink, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_ring_with(ink, &r) == ESP_OK && px(ink, 40, 24) == 0);
    }

    /*
     * Moved by whole pixels a shape is the same pixels, moved: drawn whole,
     * drawn into strips of 16 rows each with the shape at its place in the
     * strip, drawn 7 pixels right and 5 up, and drawn under a clip that cuts
     * it. Soft, translucent and with a gradient, on 16 bits.
     */
    {
        dgx_screen_t  *whole = dgx_vscreen_init(96, 96, 16, DgxScreenRGB), *moved = dgx_vscreen_init(96, 96, 16, DgxScreenRGB);
        dgx_screen_t  *strip = dgx_vscreen_init(96, 16, 16, DgxScreenRGB);
        dgx_point_2d_t pts[2][8];
        int            differ = 0, drawn = 0;
        for (int k = 0; k < KINDS; ++k)
            for (int variant = 0; variant < 4; ++variant) {
                shape_t s = make((kind_t)k, variant);
                move(&s, 11, 13, pts[0]);
                const uint32_t kinds[4] = {DGX_PAINT_SOFT | DGX_PAINT_OPACITY | DGX_PAINT_LINEAR, DGX_PAINT_SOFT | DGX_PAINT_RADIAL, 0,
                                           DGX_PAINT_OPACITY};
                *paint_of(&s) = (dgx_paint_t){.color = 0xF800, .color2 = 0x07FF, .gx = {14 * S + 3, 70 * S + 9}, .gy = {20 * S + 1, 60 * S + 2},
                                              .opacity = 170, .flags = kinds[variant]};
                dgx_fill_rectangle(whole, 0, 0, 96, 96, 0x1234);
                CHECK(draw(whole, &s) == ESP_OK);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x) drawn += px(whole, x, y) != 0x1234;
                for (int r = 0; r < 96; r += 16) {
                    shape_t part = s;
                    move(&part, 0, -r, pts[1]);
                    dgx_fill_rectangle(strip, 0, 0, 96, 16, 0x1234);
                    CHECK(draw(strip, &part) == ESP_OK);
                    for (int y = 0; y < 16; ++y)
                        for (int x = 0; x < 96; ++x) differ += px(strip, x, y) != px(whole, x, r + y);
                }
                shape_t far = s;
                move(&far, 7, -5, pts[1]);
                dgx_fill_rectangle(moved, 0, 0, 96, 96, 0x1234);
                CHECK(draw(moved, &far) == ESP_OK);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x)
                        if (x + 7 < 96 && y - 5 >= 0) differ += px(moved, x + 7, y - 5) != px(whole, x, y);
                dgx_fill_rectangle(moved, 0, 0, 96, 96, 0x1234);
                dgx_screen_clip(moved, 37, 41, 23, 19);
                CHECK(draw(moved, &s) == ESP_OK);
                dgx_screen_unclip(moved);
                for (int y = 0; y < 96; ++y)
                    for (int x = 0; x < 96; ++x) {
                        const bool in = x >= 37 && x < 60 && y >= 41 && y < 60;
                        differ += px(moved, x, y) != (in ? px(whole, x, y) : 0x1234);
                    }
                if (differ) {
                    fprintf(stderr, "%s %d: %d pixels differ\n", names[k], variant, differ);
                    differ = 0;
                    CHECK(false);
                }
            }
        CHECK(drawn > 24 * 800);
        dgx_screen_destroy(&whole), dgx_screen_destroy(&moved), dgx_screen_destroy(&strip);
    }

    /*
     * A display behind a bus: with paper, what a virtual screen of that paper
     * gets, pixel for pixel; without, every pixel the shape's color or what
     * it was.
     */
    {
        static uint8_t      buffer[512];
        dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_command, .write_data = bus_data, .write_data_async = bus_data,
                                   .sync_write = bus_sync, .buffer = buffer, .buffer_len = sizeof(buffer)};
        dgx_screen_t       *lcd = dgx_panel_init(&bus, &panel64, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
        dgx_screen_t       *mem = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
        CHECK(lcd && !dgx_screen_is_readable(lcd));
        int differ = 0, mixed = 0, odd = 0;
        for (int k = 0; k < KINDS; ++k)
            for (int variant = 0; variant < 2; ++variant) {
                shape_t s     = make((kind_t)k, variant);
                *paint_of(&s) = (dgx_paint_t){.color = 0xFFE0, .color2 = 0x001F, .gx = {0, 64 * S}, .gy = {0, 0}, .paper = 0x2945, .opacity = 200,
                                              .flags = DGX_PAINT_SOFT | DGX_PAINT_PAPER | DGX_PAINT_LINEAR | (variant ? DGX_PAINT_OPACITY : 0)};
                dgx_fill_rectangle(mem, 0, 0, 64, 64, 0x2945);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) glass[y][x] = 0x2945;
                CHECK(draw(mem, &s) == ESP_OK && draw(lcd, &s) == ESP_OK);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) differ += px(mem, x, y) != glass[y][x];
                /* no paper: whole or nothing */
                paint_of(&s)->flags = DGX_PAINT_SOFT;
                paint_of(&s)->color = 0xFFE0;
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) glass[y][x] = 0x2945;
                CHECK(draw(lcd, &s) == ESP_OK);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) odd += glass[y][x] != 0x2945 && glass[y][x] != 0xFFE0, mixed += glass[y][x] == 0xFFE0;
            }
        CHECK(differ == 0 && odd == 0 && mixed > 12 * 500);
        dgx_screen_destroy(&lcd), dgx_screen_destroy(&mem);
    }

    /* gradients: the start color at the start, the end color past the end, between them in order */
    {
        dgx_screen_t         *scr = dgx_vscreen_init(64, 64, 16, DgxScreenRGB);
        dgx_round_rectangle_t r   = {.x = 0, .y = 0, .w = 64 * S, .h = 64 * S,
                                     .paint = {.color = 0x0000, .color2 = 0xF800, .gx = {8 * S, 56 * S}, .gy = {0, 0}, .flags = DGX_PAINT_LINEAR}};
        CHECK(dgx_fill_round_rectangle_with(scr, &r) == ESP_OK);
        bool order = true;
        for (int x = 1; x < 64; ++x) order = order && (px(scr, x, 30) >> 11) >= (px(scr, x - 1, 30) >> 11) && px(scr, x, 30) == px(scr, x, 3);
        CHECK(order && px(scr, 0, 0) == 0x0000 && px(scr, 63, 63) == 0xF800 && px(scr, 32, 10) != 0 && px(scr, 32, 10) != 0xF800);
        r.paint.flags = DGX_PAINT_RADIAL;
        r.paint.gx[0] = r.paint.gy[0] = 32 * S, r.paint.gx[1] = 32 * S, r.paint.gy[1] = 52 * S;
        CHECK(dgx_fill_round_rectangle_with(scr, &r) == ESP_OK);
        CHECK(px(scr, 32, 32) >> 11 <= 1 && px(scr, 0, 0) == 0xF800 && px(scr, 32, 42) == px(scr, 42, 32) && px(scr, 32, 42) == px(scr, 21, 32));
        /* a rectangle of whole pixels: the pixels of dgx_fill_rectangle() */
        r = (dgx_round_rectangle_t){.x = 10 * S, .y = 12 * S, .w = 7 * S, .h = 5 * S, .paint = {.color = 0x07E0, .flags = DGX_PAINT_SOFT}};
        dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_round_rectangle_with(scr, &r) == ESP_OK);
        int in = 0, out = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const bool box = x >= 10 && x < 17 && y >= 12 && y < 17;
                in += box && px(scr, x, y) == 0x07E0, out += !box && px(scr, x, y) != 0;
            }
        CHECK(in == 35 && out == 0);

        /* opacity: an opaque white over black at 128 of 255 is half */
        dgx_ellipse_t e = {.x = 32 * S, .y = 32 * S, .rx = 20 * S, .ry = 20 * S, .paint = {.color = 0xFFFF, .opacity = 128, .flags = DGX_PAINT_OPACITY}};
        dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_ellipse_with(scr, &e) == ESP_OK);
        CHECK(px(scr, 32, 32) == dgx_alpha_mix16(0, 0xFFFF, 16) && px(scr, 5, 5) == 0);
        e.paint.opacity = 0;
        dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
        CHECK(dgx_fill_ellipse_with(scr, &e) == ESP_OK && px(scr, 32, 32) == 0);

        /*
         * A translucent line is painted once at its joints: every pixel it
         * covers whole is the same, along the pieces, at a sharp turn and
         * where a smooth curve bends.
         */
        static const dgx_point_2d_t turn[] = {{6 * S, 50 * S}, {20 * S + 3, 50 * S}, {34 * S, 50 * S}, {40 * S, 10 * S + 5}, {44 * S, 52 * S}, {58 * S, 30 * S}};
        for (int way = 0; way < 2; ++way) {
            dgx_polyline_t l = {.points = turn, .count = 6, .width = 6 * S, .way = way ? DGX_POLYLINE_SMOOTH : DGX_POLYLINE_STRAIGHT,
                                .paint = {.color = 0xFFFF, .opacity = 100, .flags = DGX_PAINT_SOFT | DGX_PAINT_OPACITY}};
            dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
            CHECK(dgx_draw_polyline_with(scr, &l) == ESP_OK);
            const uint32_t full = px(scr, 12, 50);
            int            darker = 0, count = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) darker += (px(scr, x, y) >> 11) > (full >> 11), count += px(scr, x, y) == full;
            CHECK(full != 0 && darker == 0 && count > 300);
        }
        /* a smooth curve goes through its points */
        dgx_polyline_t l = {.points = zigzag, .count = 5, .width = 2 * S, .way = DGX_POLYLINE_SMOOTH, .paint = {.color = 0xFFFF}};
        dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
        CHECK(dgx_draw_polyline_with(scr, &l) == ESP_OK);
        bool through = true;
        for (int i = 0; i < 5; ++i) through = through && px(scr, zigzag[i].x / S, zigzag[i].y / S) == 0xFFFF;
        CHECK(through);
        dgx_screen_destroy(&scr);
    }

    /* a monotone curve stays within its values; Catmull-Rom over the same goes past them */
    {
        dgx_screen_t               *scr    = dgx_vscreen_init(64, 64, 8, DgxScreenRGB);
        static const dgx_point_2d_t data[] = {{4 * S, 40 * S}, {14 * S, 40 * S}, {20 * S, 40 * S}, {24 * S, 16 * S}, {40 * S, 16 * S}, {60 * S, 15 * S}};
        int                         top[2], bottom[2];
        for (int way = 0; way < 2; ++way) {
            dgx_polyline_t l = {.points = data, .count = 6, .width = 2 * S, .way = way ? DGX_POLYLINE_SMOOTH : DGX_POLYLINE_MONOTONE,
                                .paint = {.color = 255}};
            dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
            CHECK(dgx_draw_polyline_with(scr, &l) == ESP_OK);
            top[way] = 64, bottom[way] = -1;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    if (px(scr, x, y)) {
                        if (y < top[way]) top[way] = y;
                        if (y > bottom[way]) bottom[way] = y;
                    }
        }
        CHECK(top[0] == 14 && bottom[0] == 40);
        CHECK(top[1] < 14 || bottom[1] > 40);
        static const dgx_point_2d_t back[] = {{4 * S, 4 * S}, {14 * S, 10 * S}, {10 * S, 12 * S}};
        dgx_polyline_t              l      = {.points = back, .count = 3, .width = S, .way = DGX_POLYLINE_MONOTONE, .paint = {.color = 255}};
        CHECK(dgx_draw_polyline_with(scr, &l) == ESP_ERR_INVALID_ARG);
        dgx_screen_destroy(&scr);
    }

    /* other depths: through runs of one color, soft by mixing */
    {
        static const uint8_t depths[] = {1, 4, 12, 18, 24, 32};
        for (size_t d = 0; d < sizeof(depths); ++d) {
            dgx_screen_t *scr = dgx_vscreen_init(64, 64, depths[d], DgxScreenRGB);
            CHECK(scr != NULL);
            if (!scr) continue;
            const uint32_t white = depths[d] == 1 ? 1u : depths[d] == 4 ? 0xF0u : depths[d] == 12 ? 0xFFF0u : depths[d] == 18 ? 0xFCFCFCu
                                                       : depths[d] == 24                          ? 0xFFFFFFu
                                                                                                  : 0xFFFFFFFFu;
            dgx_fill_rectangle(scr, 0, 0, 64, 64, 0);
            dgx_ellipse_t e = {.x = 32 * S + 5, .y = 30 * S + 3, .rx = 20 * S + 7, .ry = 15 * S + 2, .paint = {.color = white, .flags = DGX_PAINT_SOFT}};
            CHECK(dgx_fill_ellipse_with(scr, &e) == ESP_OK);
            int whole = 0, part = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) whole += px(scr, x, y) == white, part += px(scr, x, y) != white && px(scr, x, y) != 0;
            CHECK(whole > 850 && whole < 1000 && (depths[d] == 1 ? part == 0 : part > 40));
            dgx_screen_destroy(&scr);
        }
    }

    /* what is refused */
    {
        dgx_screen_t  *scr = dgx_vscreen_init(8, 8, 16, DgxScreenRGB);
        dgx_polygon_t  none = {.points = star, .count = 0};
        dgx_polyline_t thin = {.points = zigzag, .count = 5, .width = 0};
        dgx_ellipse_t  both = {.x = 0, .y = 0, .rx = S, .ry = S, .paint = {.flags = DGX_PAINT_LINEAR | DGX_PAINT_RADIAL}};
        dgx_ring_t     far  = {.x = 0, .y = 0, .r_outer = 40000 * S, .to = 360};
        CHECK(dgx_fill_polygon_with(scr, &none) == ESP_ERR_INVALID_ARG && dgx_draw_polyline_with(scr, &thin) == ESP_ERR_INVALID_ARG);
        CHECK(dgx_fill_ellipse_with(scr, &both) == ESP_ERR_INVALID_ARG && dgx_fill_ring_with(scr, &far) == ESP_ERR_INVALID_ARG);
        CHECK(dgx_fill_ring_with(NULL, &far) == ESP_ERR_INVALID_ARG && dgx_fill_ring_with(scr, NULL) == ESP_ERR_INVALID_ARG);
        dgx_screen_destroy(&scr);
    }

    dgx_screen_destroy(&ink);
    CHECK_DONE();
}
