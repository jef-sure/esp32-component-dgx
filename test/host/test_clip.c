/*
 * The clip of a screen, dgx_screen_clip(): every call that changes pixels keeps to it.
 *
 * Each drawing is made twice, on a screen with a clip and on one without, both
 * holding the same picture before it. Inside the clip the two must be alike,
 * outside it the clipped screen must still hold the picture it had, and the
 * area it marks as changed must lie inside the clip. The screens are virtual
 * ones of every depth, a monochrome screen of pages, and a model of a 16-bit
 * panel behind a bus, which cannot be read and gets its pixels through windows.
 * The clips: one inside the screen, one at its corner, one partly off it, and
 * an empty one, which lets nothing change.
 */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_gauge.h"
#include "dgx_hw_font.h"
#include "dgx_image.h"
#include "dgx_morph_render.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "dgx_vscreen.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/font0ss.h"

#define W 64
#define H 48

/* ---- a 16-bit panel behind a bus: its memory, written through windows as a controller writes it ---- */

typedef struct {
    dgx_screen_with_bus_t sbus; /* first: the screen is the panel */
    dgx_bus_protocols_t   bus;
    uint8_t               buffer[128];
    uint16_t              mem[H][W];
    int                   left, right, top, bottom, x, y;
} panel_t;

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    panel_t *p = (panel_t *)scr;
    if (left > right) DGX_INT_SWAP(left, right);
    if (top > bottom) DGX_INT_SWAP(top, bottom);
    p->left = left, p->right = right, p->top = top, p->bottom = bottom, p->x = left, p->y = top;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    panel_t *p = (panel_t *)scr;
    for (uint32_t i = 0; i < lenbits / 16u; ++i) {
        if (p->x < W && p->y < H) p->mem[p->y][p->x] = (uint16_t)(data[2 * i] << 8 | data[2 * i + 1]);
        if (++p->x > p->right) {
            p->x = p->left;
            if (++p->y > p->bottom) p->y = p->top;
        }
    }
}

static void panel_wait(dgx_screen_t *scr) { (void)scr; }

static void quiet(dgx_screen_t *scr, int left, int right, int top, int bottom) { (void)scr, (void)left, (void)right, (void)top, (void)bottom; }

static dgx_screen_t *panel_make(void)
{
    panel_t *p = calloc(1, sizeof(*p));
    p->bus.buffer = p->buffer, p->bus.buffer_len = sizeof(p->buffer);
    p->sbus.bus       = &p->bus;
    dgx_screen_t *scr = &p->sbus.scr;
    scr->width = W, scr->height = H, scr->color_bits = 16, scr->screen_subtype = DgxPhysicalScreenWithBus;
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->set_area = panel_set_area, scr->write_area = panel_write_area, scr->wait_buffer = panel_wait, scr->update_screen = quiet;
    scr->destroy = NULL;
    return scr;
}

/* ---- the screens ---- */

enum { KINDS = 8 };
static const char *const kind_name[KINDS] = {"vscreen 1", "vscreen 8", "vscreen 12", "vscreen 16", "vscreen 18", "vscreen 24", "pages", "panel"};

static dgx_screen_t *make(int kind)
{
    static const uint8_t bits[] = {1, 8, 12, 16, 18, 24};
    if (kind < 6) return dgx_vscreen_init(W, H, bits[kind], DgxScreenRGB);
    if (kind == 6) return dgx_bw_init(W, H);
    return panel_make();
}

static void unmake(dgx_screen_t **scr)
{
    if ((*scr)->screen_subtype == DgxPhysicalScreenWithBus) {
        free(*scr);
        *scr = NULL;
        return;
    }
    dgx_screen_destroy(scr);
}

static uint32_t pixel(dgx_screen_t *scr, int x, int y)
{
    if (scr->screen_subtype == DgxPhysicalScreenWithBus) return ((panel_t *)scr)->mem[y][x];
    return scr->get_pixel(scr, x, y);
}

static uint32_t mask_of(const dgx_screen_t *scr)
{
    return scr->color_bits >= 32 ? 0xffffffffu : (1u << scr->color_bits) - 1u;
}

/* a picture that differs from pixel to pixel, so that a pixel moved or left out shows */
static void paint(dgx_screen_t *scr)
{
    dgx_screen_batch_begin(scr);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint32_t v = (uint32_t)(x * 2654435761u ^ y * 40503u) >> 7;
            scr->set_pixel(scr, x, y, v & mask_of(scr));
        }
    int l, t, w, h;
    dgx_screen_take_dirty(scr, &l, &t, &w, &h);
    dgx_screen_batch_end(scr);
}

/* a color of the screen, its own for each use */
static uint32_t ink(const dgx_screen_t *scr, int k)
{
    return ((0x9e3779b9u * (uint32_t)(k + 3)) >> 5 & mask_of(scr)) | (scr->color_bits == 1 ? 1u : 0u);
}

/* ---- the drawings ---- */

static void op_pixels(dgx_screen_t *s)
{
    for (int y = -3; y < H + 3; y += 3)
        for (int x = -2; x < W + 2; x += 2) dgx_set_pixel(s, x, y, ink(s, x + y));
}

static void op_fills(dgx_screen_t *s)
{
    dgx_fill_rectangle(s, 5, 4, 40, 30, ink(s, 1));
    dgx_fill_rectangle(s, 60, 40, -30, -25, ink(s, 2));
    dgx_fill_rectangle(s, -10, 20, 200, 1, ink(s, 3));
    dgx_fill_rectangle(s, 33, -5, 1, 100, ink(s, 4));
    dgx_fill_rectangle(s, 10, 10, 2, 2, ink(s, 5));
}

static void op_whole_fill(dgx_screen_t *s) { dgx_fill_rectangle(s, 0, 0, W, H, 0); }

static void op_lines(dgx_screen_t *s)
{
    for (int k = 0; k < 16; ++k) {
        const int x = (k * 37) % (W + 20) - 10, y = (k * 23) % (H + 20) - 10;
        dgx_draw_line(s, 31, 22, x, y, ink(s, k));
    }
    dgx_draw_line(s, -20, -5, W + 30, H + 7, ink(s, 20));
    dgx_draw_line(s, 3, 40, 60, 40, ink(s, 21));
    dgx_draw_line(s, 50, 2, 50, 45, ink(s, 22));
    dgx_draw_line_mask(s, 0, 47, 63, 0, ink(s, 23), ink(s, 24), 0xf0f0f0f0u, 32);
}

static void op_thick_lines(dgx_screen_t *s)
{
    static const int widths[] = {2, 3, 5, 4 | DGX_PEN_SOFT, 1};
    for (int k = 0; k < 5; ++k) {
        dgx_draw_thick_line(s, 3 + k, 5 + 7 * k, 60 - 3 * k, 44 - 5 * k, widths[k], ink(s, k));
        dgx_draw_thick_line(s, 10 + 9 * k, 2, 12 + 9 * k, 46, widths[k], ink(s, k + 5));
    }
    dgx_draw_thick_line(s, 40, 30, 40, 30, 7, ink(s, 11));
}

static void op_rectangles(dgx_screen_t *s)
{
    dgx_draw_rectangle(s, 4, 3, 50, 38, ink(s, 1));
    dgx_draw_round_rectangle(s, 8, 6, 44, 30, 7, ink(s, 2));
    dgx_fill_round_rectangle(s, 20, 15, 40, 30, 9, ink(s, 3));
    dgx_fill_round_rectangle(s, -10, -10, 30, 25, 5, ink(s, 4));
}

static void op_circles(dgx_screen_t *s)
{
    dgx_draw_circle(s, 30, 22, 18, ink(s, 1));
    dgx_fill_circle(s, 18, 30, 11, ink(s, 2));
    dgx_fill_circle(s, 45, 15, 9 | DGX_PEN_SOFT, s->color_bits == 8 ? 200 : ink(s, 3));
    dgx_fill_circle(s, 60, 44, 14, ink(s, 4));
    dgx_draw_circle(s, 2, 2, 25, ink(s, 5));
}

static void op_ellipses(dgx_screen_t *s)
{
    dgx_draw_ellipse(s, 32, 24, 28, 15, ink(s, 1));
    dgx_fill_ellipse(s, 20, 30, 15, 9, ink(s, 2));
    dgx_fill_sector(s, 40, 20, 20, 16, 30.0f, 250.0f, ink(s, 3));
    dgx_draw_arc(s, 32, 24, 22, 18, -40.0f, 200.0f, 1, ink(s, 4));
    dgx_draw_arc(s, 30, 22, 14, 14, 10.0f, 300.0f, 3, ink(s, 5));
    dgx_draw_arc(s, 34, 26, 19, 12, 0.0f, 360.0f, 3 | DGX_PEN_SOFT, s->color_bits == 8 ? 180 : ink(s, 6));
}

static void op_polygons(dgx_screen_t *s)
{
    dgx_fill_triangle(s, 3, 40, 60, 2, 30, 47, ink(s, 1));
    dgx_draw_triangle(s, -5, 5, 70, 20, 10, 60, ink(s, 2));
    dgx_fill_polygon4(s, 10, 10, 50, 5, 55, 40, 15, 35, ink(s, 3));
    dgx_fill_polygon4(s, 5, 5, 40, 20, 60, 5, 30, 45, ink(s, 4)); /* concave */
}

static void op_pen(dgx_screen_t *s)
{
    static const int widths[] = {1, 3, 4, 5 | DGX_PEN_SOFT, 2 | DGX_PEN_SOFT};
    const dgx_point_2d_t line[] = {{2, 3}, {40, 10}, {20, 40}, {62, 45}, {60, 2}};
    for (int k = 0; k < 5; ++k) {
        const uint32_t color = (widths[k] & DGX_PEN_SOFT) && s->color_bits == 8 ? 255u - 30u * k : ink(s, k);
        dgx_draw_thick_segment(s, 1.5f + k, 44.0f - 3 * k, 62.0f - 2 * k, 4.5f + 6 * k, widths[k], color);
        const dgx_point_2d_t b3[3] = {{(int16_t)(2 + k), 46}, {32, (int16_t)(-20 + 5 * k)}, {(int16_t)(62 - k), 46}};
        const dgx_point_2d_t b4[4] = {{0, (int16_t)(10 + k)}, {20, 60}, {44, -15}, {64, (int16_t)(30 - k)}};
        dgx_draw_bezier3(s, b3, 0, widths[k], color);
        dgx_draw_bezier4(s, b4, 0, widths[k], color);
    }
    dgx_draw_polyline(s, line, 5, 3, ink(s, 9));
    dgx_draw_polyline(s, line, 5, 1, ink(s, 10));
}

static void op_text(dgx_screen_t *s)
{
    static const dgx_output_orientation_t turns[] = {DgxOutputNormal, DgxOutputRotate90CW, DgxOutputRotate180, DgxOutputMirrorX};
    for (int k = 0; k < 4; ++k) {
        const dgx_font_context_t ctx = {.scr = s, .font = TerminusTTFMedium12(), .color = ink(s, k), .bg = ink(s, k + 4),
                                        .orientation = turns[k], .scale = k == 1 ? 2 : 1, .flags = k == 2 ? DGX_FONT_FILL : 0};
        dgx_font_draw(&ctx, 6 + 10 * k, 14 + 9 * k, "Clip Hg 12");
    }
}

/* a font of 4 bits a pixel, made here: two glyphs of every level */
static uint8_t       soft_bits[2][32];
static glyph_t       soft_glyphs[2];
static glyph_array_t soft_ranges[1];
static dgx_font_t    soft_font;

static void make_soft_font(void)
{
    for (int k = 0; k < 2; ++k) {
        const int w = 6 + k, h = 7, pitch = (w * 4 + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) soft_bits[k][y * pitch + x / 2] |= (uint8_t)(((x * 5 + y * 3 + k) % 16) << (x & 1 ? 0 : 4));
        soft_glyphs[k] = (glyph_t){.bitmap = soft_bits[k], .width = (int16_t)w, .height = (int16_t)h, .xAdvance = 8, .xOffset = 0, .yOffset = -7};
    }
    soft_ranges[0] = (glyph_array_t){.first = 'A', .number = 2, .glyphs = soft_glyphs};
    soft_font      = (dgx_font_t){.glyph_ranges = soft_ranges, .yAdvance = 9, .yOffsetLowest = -7, .xWidest = 7, .xWidthAverage = 7,
                                  .f_type = DGX_FONT_BITMAP_DEPTH4, .xRightMax = 7, .number_of_ranges = 1};
}

static void op_soft_text(dgx_screen_t *s)
{
    static const uint32_t flags[] = {0, DGX_FONT_BG, DGX_FONT_FILL, DGX_FONT_HARD};
    for (int k = 0; k < 4; ++k) {
        const dgx_font_context_t ctx = {.scr = s, .font = &soft_font, .color = ink(s, k), .bg = ink(s, k + 4), .flags = flags[k],
                                        .orientation = k == 3 ? DgxOutputRotate90CCW : DgxOutputNormal, .scale = k == 1 ? 2 : 1};
        dgx_font_draw(&ctx, 2 + 9 * k, 10 + 10 * k, "ABBAABAB");
    }
}

static void op_handwriting(dgx_screen_t *s)
{
    static const int widths[] = {1, 3, 2 | DGX_PEN_SOFT};
    for (int k = 0; k < 3; ++k) {
        const uint32_t         color = (widths[k] & DGX_PEN_SOFT) && s->color_bits == 8 ? 230 : ink(s, k);
        const dgx_hw_context_t ctx   = {.scr = s, .font = font0ss(), .scale_x = 1.3f, .width = widths[k], .color = color, .joined = k == 1};
        dgx_hw_draw(&ctx, -4 + 6 * k, 20 + 12 * k, "clip me");
    }
}

static void op_images(dgx_screen_t *s)
{
    static uint8_t one[8 * 3], eight[17 * 13];
    for (size_t i = 0; i < sizeof(one); ++i) one[i] = (uint8_t)(0xa5 ^ i * 29);
    for (size_t i = 0; i < sizeof(eight); ++i) eight[i] = (uint8_t)(i * 7 % 13 ? i * 31 : 0);
    const dgx_image_t      icon = {.width = 20, .height = 8, .origin_x = 3, .origin_y = 2, .format = DGX_IMAGE_1BPP, .data = one};
    const dgx_image_blit_t how  = {.color = ink(s, 1), .orientation = DgxOutputRotate90CW};
    dgx_image_blit(s, 30, 10, &icon, &how);
    dgx_image_blit(s, 50, 40, &icon, &(dgx_image_blit_t){.color = ink(s, 2)});
    if (s->color_bits == 8 && dgx_vscreen_is_linear(s)) {
        const dgx_image_t picture = {.width = 17, .height = 13, .format = DGX_IMAGE_8BPP, .data = eight};
        dgx_image_blit(s, 5, 25, &picture, &(dgx_image_blit_t){.orientation = DgxOutputRotate180});
        dgx_image_blit(s, 40, 0, &picture, &(dgx_image_blit_t){.flags = DGX_IMAGE_CLIP, .clip_x = 30, .clip_y = 3, .clip_w = 20, .clip_h = 30});
    }
}

static void op_transfers(dgx_screen_t *s)
{
    if (s->color_bits == 1 && !dgx_vscreen_is_linear(s)) return; /* a screen of pages takes no transfer */
    dgx_screen_t *sprite = dgx_vscreen_init(20, 14, s->color_bits, DgxScreenRGB);
    for (int y = 0; y < 14; ++y)
        for (int x = 0; x < 20; ++x) dgx_set_pixel(sprite, x, y, (x + y) % 5 ? ink(s, x * y) : 0);
    dgx_vscreen_to_screen(s, 3, 4, sprite, 0);
    dgx_vscreen_to_screen(s, 30, 20, sprite, DGX_VSCREEN_TRANSPARENT);
    dgx_vscreen_to_screen(s, 20, 10, sprite, DGX_VSCREEN_SCALE(2));
    dgx_vscreen_to_screen(s, -5, 30, sprite, DGX_VSCREEN_ORIENTED(DgxOutputRotate90CW));
    dgx_vscreen_region_to_screen(s, 45, 33, sprite, 4, 2, 12, 9, DGX_VSCREEN_SCALE(2) | DGX_VSCREEN_SMOOTH);
    if (s->color_bits == 16) {
        dgx_vscreen_to_screen(s, 10, 30, sprite, DGX_VSCREEN_ALPHA(100));
        dgx_screen_t *ink8 = dgx_vscreen_init(24, 16, 8, DgxScreenRGB);
        uint16_t      lut[256];
        for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)(i * 257 ^ 0x1234);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 24; ++x) dgx_set_pixel(ink8, x, y, (uint32_t)(x * 11 + y * 5) & 0xff);
        dgx_vscreen8_to_screen16(s, 33, 2, ink8, lut, 0);
        dgx_vscreen8_to_screen16(s, 2, 25, ink8, lut, DGX_VSCREEN_TRANSPARENT | DGX_VSCREEN_SCALE(2));
        dgx_screen_destroy(&ink8);
    }
    dgx_screen_destroy(&sprite);
}

static void op_texture(dgx_screen_t *s)
{
    if (s->color_bits < 8) return;
    dgx_screen_t *texture = dgx_vscreen_init(16, 16, s->color_bits, DgxScreenRGB);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) dgx_set_pixel(texture, x, y, (uint32_t)(x * 4099 + y * 77) & mask_of(s));
    const dgx_point_2d_t quad[4] = {{5, 2}, {60, 10}, {50, 46}, {-4, 30}};
    dgx_draw_texture_quad(s, quad, texture, 0, 0, 16, 16);
    dgx_draw_texture_rect(s, 30, 25, 40, 30, texture, 2, 2, 12, 12);
    dgx_screen_destroy(&texture);
}

/* the clip of the test, which the screen without a clip is told of where a call cuts by it before it draws */
static int clip_left, clip_top, clip_right, clip_bottom;

/* the rectangle cut by the clip of the test: for the screen that has no clip of its own */
static void cut(const dgx_screen_t *s, int *x, int *y, int *w, int *h)
{
    int cx, cy, cw, ch;
    dgx_screen_clip_get(s, &cx, &cy, &cw, &ch);
    if (cw != W || ch != H) return; /* the clipped screen cuts by itself */
    int right = *x + *w, bottom = *y + *h;
    if (*x < clip_left) *x = clip_left;
    if (*y < clip_top) *y = clip_top;
    if (right > clip_right) right = clip_right;
    if (bottom > clip_bottom) bottom = clip_bottom;
    *w = right - *x, *h = bottom - *y;
}

/* a scroll moves what of its rectangle the clip has, as what the screen has: the rectangle is cut by it first */
static void op_scroll(dgx_screen_t *s)
{
    if (!dgx_screen_is_readable(s)) return;
    int x = 4, y = 6, w = 50, h = 35;
    cut(s, &x, &y, &w, &h);
    if (w > 0 && h > 0) dgx_scroll(s, x, y, w, h, 3, -4, ink(s, 1));
    x = 10, y = 0, w = 40, h = H;
    cut(s, &x, &y, &w, &h);
    if (w > 0 && h > 0) dgx_scroll_no_fill(s, x, y, w, h, -6, 2);
}

static uint32_t gauge_color(int value) { return (uint32_t)value * 977u; }

static void op_gauge(dgx_screen_t *s)
{
    dgx_gauge_t gauge;
    dgx_gauge_init(&gauge, s, 32, 24, 12, 6, 225.0f, 270, 0, 100, 0, gauge_color);
    dgx_gauge_set_value(&gauge, 70);
}

static void op_copy(dgx_screen_t *s)
{
    if (!dgx_vscreen_is_linear(s)) return;
    dgx_screen_t *other = dgx_vscreen_init(W, H, s->color_bits, DgxScreenRGB);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) dgx_set_pixel(other, x, y, ink(s, x ^ y));
    CHECK(dgx_vscreen_copy(s, other));
    dgx_screen_destroy(&other);
}

static void op_glow(dgx_screen_t *s)
{
    if (s->color_bits != 16 && s->color_bits != 24) return;
    dgx_morph_glow_t *glow = dgx_morph_glow_create(40, 30, 2, s->color_bits);
    if (!glow) return;
    for (int k = 0; k < 12; ++k) dgx_morph_glow_dot(glow, &(dgx_point_2d_t){.x = (int16_t)(3 * k), .y = (int16_t)(2 * k + 3)}, 255);
    dgx_morph_glow_present(glow, 1.0f, s, 20, 15);
    dgx_morph_glow_present(glow, 1.0f, s, -10, -8);
    dgx_morph_glow_destroy(&glow);
}

typedef struct {
    const char *name;
    void (*draw)(dgx_screen_t *s);
} op_t;

static const op_t ops[] = {
    {"pixels", op_pixels},     {"fills", op_fills},     {"whole fill", op_whole_fill}, {"lines", op_lines},
    {"thick lines", op_thick_lines}, {"rectangles", op_rectangles}, {"circles", op_circles}, {"ellipses", op_ellipses},
    {"polygons", op_polygons}, {"pen", op_pen},         {"text", op_text},             {"soft text", op_soft_text},
    {"handwriting", op_handwriting}, {"images", op_images}, {"transfers", op_transfers}, {"texture", op_texture},
    {"scroll", op_scroll},     {"gauge", op_gauge},     {"copy", op_copy},             {"glow", op_glow},
};

/* ---- the comparison ---- */

typedef struct {
    int x, y, w, h;          /* as asked */
    int left, top, right, bottom; /* as it is on the screen, right and bottom past it */
} clip_t;

static const clip_t clips[] = {
    {13, 9, 30, 22, 13, 9, 43, 31},
    {0, 0, 21, 48, 0, 0, 21, 48},
    {40, -7, 50, 30, 40, 0, 64, 23},
    {20, 20, 0, 10, 0, 0, 0, 0},
};

static int drawn_by[sizeof(ops) / sizeof(ops[0])];

static void check_op(const op_t *op, int kind, const clip_t *c)
{
    dgx_screen_t *a = make(kind), *b = make(kind);
    paint(a), paint(b);
    clip_left = c->left, clip_top = c->top, clip_right = c->right, clip_bottom = c->bottom;
    dgx_screen_clip(a, c->x, c->y, c->w, c->h);
    int cx, cy, cw, ch;
    dgx_screen_clip_get(a, &cx, &cy, &cw, &ch);
    CHECK(cx == c->left || cw == 0);
    CHECK(cw == c->right - c->left && ch == c->bottom - c->top);

    dgx_screen_batch_begin(a);
    op->draw(a);
    int dl, dt, dw, dh;
    const bool dirty = dgx_screen_take_dirty(a, &dl, &dt, &dw, &dh);
    dgx_screen_batch_end(a);
    op->draw(b);

    /* the clip is still what it was */
    int ax, ay, aw, ah;
    dgx_screen_clip_get(a, &ax, &ay, &aw, &ah);
    CHECK(ax == cx && ay == cy && aw == cw && ah == ch);

    dgx_screen_t *picture = make(kind);
    paint(picture);
    int wrong_in = 0, wrong_out = 0, drawn_in = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool     in = x >= c->left && x < c->right && y >= c->top && y < c->bottom;
            const uint32_t pa = pixel(a, x, y), pb = pixel(b, x, y), was = pixel(picture, x, y);
            if (in) {
                wrong_in += pa != pb;
                drawn_in += pb != was;
            } else {
                wrong_out += pa != was;
            }
        }
    if (wrong_in || wrong_out) {
        fprintf(stderr, "%s on %s, clip %d,%d %dx%d: %d pixels inside unlike the unclipped screen, %d outside changed\n", op->name, kind_name[kind],
                c->x, c->y, c->w, c->h, wrong_in, wrong_out);
    }
    CHECK(wrong_in == 0 && wrong_out == 0);
    if (dirty) {
        const bool inside = dl >= c->left && dt >= c->top && dl + dw <= c->right && dt + dh <= c->bottom;
        if (!inside) fprintf(stderr, "%s on %s: changed area %d,%d %dx%d outside the clip\n", op->name, kind_name[kind], dl, dt, dw, dh);
        CHECK(inside);
    }
    /* a drawing that changed something inside the clip has marked it */
    if (drawn_in && kind != KINDS - 1) CHECK(dirty);
    drawn_by[op - ops] += drawn_in;
    unmake(&a), unmake(&b), unmake(&picture);
}

/* ---- the calls themselves ---- */

static void test_calls(void)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    int           x, y, w, h;
    dgx_screen_clip_get(s, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == W && h == H);
    dgx_screen_clip(s, -5, 10, 30, 100);
    dgx_screen_clip_get(s, &x, &y, &w, &h);
    CHECK(x == 0 && y == 10 && w == 25 && h == H - 10);
    /* one clip takes the place of the one before */
    dgx_screen_clip(s, 50, 2, 30, 4);
    dgx_screen_clip_get(s, &x, &y, &w, &h);
    CHECK(x == 50 && y == 2 && w == W - 50 && h == 4);
    /* off the screen: nothing may change */
    dgx_screen_clip(s, W, 0, 10, 10);
    dgx_screen_clip_get(s, &x, &y, &w, &h);
    CHECK(w == 0 && h == 0);
    dgx_screen_unclip(s);
    dgx_screen_clip_get(s, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == W && h == H);
    /* the area marked as changed is cut by the clip */
    dgx_screen_clip(s, 10, 10, 20, 20);
    dgx_screen_batch_begin(s);
    dgx_screen_touch(s, 0, W - 1, 0, H - 1);
    CHECK(dgx_screen_take_dirty(s, &x, &y, &w, &h) && x == 10 && y == 10 && w == 20 && h == 20);
    dgx_screen_batch_end(s);
    dgx_screen_destroy(&s);

    /* a new size gives back the whole screen */
    static uint16_t pixels[2][W * H];
    s = dgx_vscreen_from_array(pixels[0], W, H, 16, DgxScreenRGB);
    dgx_screen_clip(s, 1, 2, 3, 4);
    dgx_vscreen_array_set(s, pixels[1], H, W);
    dgx_screen_clip_get(s, &x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == H && h == W);
    dgx_screen_destroy(&s);
}

int main(void)
{
    make_soft_font();
    test_calls();
    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o)
        for (int kind = 0; kind < KINDS; ++kind)
            for (size_t c = 0; c < sizeof(clips) / sizeof(clips[0]); ++c) check_op(&ops[o], kind, &clips[c]);
    /* every drawing has drawn inside a clip somewhere, or it proves nothing */
    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
        if (!drawn_by[o]) fprintf(stderr, "%s drew nothing inside any clip\n", ops[o].name);
        CHECK(drawn_by[o] > 0);
    }
    CHECK_DONE();
}
