/*
 * Pictures and text that put every pixel of their box once: DGX_IMAGE_OPAQUE
 * and the field of a text, dgx_font_context_t::field.
 *
 * An opaque picture puts what the same picture puts over its box filled with
 * bg, on a virtual screen of 8 bits by values and maps, and in one color on
 * screens of 16 bits and of pages; on a model of a 16-bit panel behind a bus
 * the box is written once, pixel by pixel, in one window. A text in a field
 * puts what the field filled with the paper and the text drawn over it put,
 * at its start, middle or end, cut by the field, in every orientation and at
 * three scales; on the model panel every pixel of the field is written once;
 * a shorter text drawn over a longer one leaves nothing of it.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_image.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "dgx_vscreen.h"
#include "fonts/TerminusTTFMedium12.h"

#define W 96
#define H 64

static uint32_t seed = 31337;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

/* ---- a 16-bit panel behind a bus: its memory, how many times each pixel was written, and the windows ---- */

typedef struct {
    dgx_screen_with_bus_t sbus; /* first: the screen is the panel */
    dgx_bus_protocols_t   bus;
    uint8_t               buffer[256];
    uint16_t              mem[H][W];
    uint8_t               writes[H][W];
    int                   windows;
    int                   left, right, top, bottom, x, y;
} panel_t;

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    panel_t *p = (panel_t *)scr;
    if (left > right) DGX_INT_SWAP(left, right);
    if (top > bottom) DGX_INT_SWAP(top, bottom);
    p->left = left, p->right = right, p->top = top, p->bottom = bottom, p->x = left, p->y = top;
    ++p->windows;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    panel_t *p = (panel_t *)scr;
    for (uint32_t i = 0; i < lenbits / 16u; ++i) {
        if (p->x < W && p->y < H) {
            p->mem[p->y][p->x] = (uint16_t)(data[2 * i] << 8 | data[2 * i + 1]);
            if (p->writes[p->y][p->x] < 255) ++p->writes[p->y][p->x];
        }
        if (++p->x > p->right) {
            p->x = p->left;
            if (++p->y > p->bottom) p->y = p->top;
        }
    }
}

static void panel_wait(dgx_screen_t *scr) { (void)scr; }

static void quiet(dgx_screen_t *scr, int left, int right, int top, int bottom) { (void)scr, (void)left, (void)right, (void)top, (void)bottom; }

static panel_t *panel_make(size_t buffer_len)
{
    panel_t *p = calloc(1, sizeof(*p));
    p->bus.buffer = p->buffer, p->bus.buffer_len = buffer_len;
    p->sbus.bus       = &p->bus;
    dgx_screen_t *scr = &p->sbus.scr;
    scr->width = W, scr->height = H, scr->color_bits = 16, scr->screen_subtype = DgxPhysicalScreenWithBus;
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->set_area = panel_set_area, scr->write_area = panel_write_area, scr->wait_buffer = panel_wait, scr->update_screen = quiet;
    return p;
}

/* the picture of the panel put into a virtual screen of 16 bits, and the counts cleared */
static void panel_paint(panel_t *p, uint16_t value)
{
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) p->mem[y][x] = value;
    memset(p->writes, 0, sizeof(p->writes));
    p->windows = 0;
}

static int differ_panel(const panel_t *p, dgx_screen_t *v)
{
    int n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) n += p->mem[y][x] != dgx_get_pixel(v, x, y);
    return n;
}

static int differ(dgx_screen_t *a, dgx_screen_t *b)
{
    int n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) n += dgx_get_pixel(a, x, y) != dgx_get_pixel(b, x, y);
    return n;
}

/* every pixel of the rectangle written once, none outside it */
static bool once_in(const panel_t *p, int left, int top, int right, int bottom)
{
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool in = x >= left && x < right && y >= top && y < bottom;
            if (p->writes[y][x] != (in ? 1 : 0)) return false;
        }
    return true;
}

static void paint(dgx_screen_t *scr, uint32_t value)
{
    dgx_screen_unclip(scr);
    dgx_fill_rectangle(scr, 0, 0, W, H, value);
}

/* ---- opaque pictures ---- */

/* where the box of the turned picture lies, as dgx_image_blit() puts it */
static void image_box(const dgx_image_t *im, dgx_output_orientation_t turn, int x, int y, int *bx, int *by, int *bw, int *bh)
{
    const bool swapped = turn == DgxOutputTranspose || turn == DgxOutputRotate90CCW || turn == DgxOutputRotate90CW || turn == DgxOutputTransverse;
    const bool x_back  = turn == DgxOutputMirrorX || turn == DgxOutputRotate180 || turn == DgxOutputRotate90CCW || turn == DgxOutputTransverse;
    const bool y_back  = turn == DgxOutputMirrorY || turn == DgxOutputRotate180 || turn == DgxOutputRotate90CW || turn == DgxOutputTransverse;
    const int  ox = x_back ? im->width - 1 - im->origin_x : im->origin_x, oy = y_back ? im->height - 1 - im->origin_y : im->origin_y;
    *bx = x - (swapped ? oy : ox), *by = y - (swapped ? ox : oy);
    *bw = swapped ? im->height : im->width, *bh = swapped ? im->width : im->height;
}

static void test_opaque_pictures(void)
{
    static uint8_t data[40 * 40];
    static uint8_t map[255];
    for (size_t i = 0; i < sizeof(map); ++i) map[i] = (uint8_t)(200 - i % 150);
    dgx_screen_t *a8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB), *b8 = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    dgx_screen_t *a16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b16 = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    panel_t      *panel = panel_make(sizeof(((panel_t *)0)->buffer)), *narrow = panel_make(16);
    static const uint8_t formats[] = {DGX_IMAGE_1BPP, DGX_IMAGE_4BPP, DGX_IMAGE_8BPP};
    for (int round = 0; round < 3000; ++round) {
        const uint8_t format = formats[round % 3];
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(rnd(0, 3) ? rnd(0, 255) : 0);
        const dgx_image_t im = {.width = (uint16_t)rnd(1, 30), .height = (uint16_t)rnd(1, 30), .origin_x = (int16_t)rnd(-3, 10),
                                .origin_y = (int16_t)rnd(-3, 10), .format = format, .data = data};
        const dgx_output_orientation_t turn = (dgx_output_orientation_t)rnd(0, 7);
        const int x = rnd(-20, W + 5), y = rnd(-20, H + 5);
        int       bx, by, bw, bh;
        image_box(&im, turn, x, y, &bx, &by, &bw, &bh);
        const bool clipped = round % 4 == 0;
        const int  cx = rnd(0, W / 2), cy = rnd(0, H / 2), cw = rnd(1, W / 2), ch = rnd(1, H / 2);

        /* by values or a map, on an 8-bit virtual screen: the box takes bg, then the pixels that are not 0 */
        dgx_image_blit_t how = {.flags = DGX_IMAGE_OPAQUE, .map = round % 2 ? map : NULL, .color = 77, .orientation = turn, .bg = 9};
        paint(a8, 250), paint(b8, 250);
        if (clipped) dgx_screen_clip(a8, cx, cy, cw, ch), dgx_screen_clip(b8, cx, cy, cw, ch);
        CHECK(dgx_image_blit(a8, x, y, &im, &how) == ESP_OK);
        dgx_fill_rectangle(b8, bx, by, bw, bh, 9);
        how.flags = 0;
        CHECK(dgx_image_blit(b8, x, y, &im, &how) == ESP_OK);
        CHECK(differ(a8, b8) == 0);

        /* in one color on a 16-bit virtual screen, by runs of the two colors */
        const uint32_t color = (uint32_t)rnd(1, 0xffff), bg = (uint32_t)rnd(0, 0xffff) ^ color;
        how = (dgx_image_blit_t){.flags = DGX_IMAGE_OPAQUE | (format == DGX_IMAGE_1BPP ? 0 : DGX_IMAGE_SILHOUETTE), .color = color, .orientation = turn,
                                 .bg = bg};
        paint(a16, 0x1234), paint(b16, 0x1234);
        if (clipped) dgx_screen_clip(a16, cx, cy, cw, ch), dgx_screen_clip(b16, cx, cy, cw, ch);
        CHECK(dgx_image_blit(a16, x, y, &im, &how) == ESP_OK);
        dgx_fill_rectangle(b16, bx, by, bw, bh, bg);
        dgx_image_blit_t over = how;
        over.flags &= ~DGX_IMAGE_OPAQUE;
        CHECK(dgx_image_blit(b16, x, y, &im, &over) == ESP_OK);
        CHECK(differ(a16, b16) == 0);

        /* on a display behind a bus: the same picture, the box written once, in one window */
        int l = DGX_MAX(bx, 0), t = DGX_MAX(by, 0), r = DGX_MIN(bx + bw, W), b = DGX_MIN(by + bh, H);
        if (clipped) l = DGX_MAX(l, cx), t = DGX_MAX(t, cy), r = DGX_MIN(r, cx + cw), b = DGX_MIN(b, cy + ch);
        dgx_screen_t *lcd = &panel->sbus.scr;
        panel_paint(panel, 0x1234);
        dgx_screen_unclip(lcd);
        if (clipped) dgx_screen_clip(lcd, cx, cy, cw, ch);
        CHECK(dgx_image_blit(lcd, x, y, &im, &how) == ESP_OK);
        CHECK(differ_panel(panel, a16) == 0);
        if (r > l && b > t) {
            CHECK(once_in(panel, l, t, r, b));
            CHECK(panel->windows == 1);
        } else {
            CHECK(panel->windows == 0);
        }
        /* a row too long for the buffer of the bus: by runs, still once */
        lcd = &narrow->sbus.scr;
        panel_paint(narrow, 0x1234);
        dgx_screen_unclip(lcd);
        if (clipped) dgx_screen_clip(lcd, cx, cy, cw, ch);
        CHECK(dgx_image_blit(lcd, x, y, &im, &how) == ESP_OK);
        CHECK(differ_panel(narrow, a16) == 0);
        if (r > l && b > t) CHECK(once_in(narrow, l, t, r, b));
    }
    dgx_screen_destroy(&a8), dgx_screen_destroy(&b8), dgx_screen_destroy(&a16), dgx_screen_destroy(&b16);
    free(panel), free(narrow);
}

/* ---- text in a field ---- */

/* a font of 4 bits a pixel, made here: glyphs inside their advances, so that one pass and two give the same pixels */
static uint8_t       soft_bits[3][40];
static glyph_t       soft_glyphs[3];
static glyph_array_t soft_ranges[1];
static dgx_font_t    soft_font;

static void make_soft_font(void)
{
    for (int k = 0; k < 3; ++k) {
        const int w = 5 + k, h = 8, pitch = (w * 4 + 7) / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) soft_bits[k][y * pitch + x / 2] |= (uint8_t)(((x * 7 + y * 5 + k * 3) % 16) << (x & 1 ? 0 : 4));
        soft_glyphs[k] = (glyph_t){.bitmap = soft_bits[k], .width = (int16_t)w, .height = (int16_t)h, .xAdvance = 8, .xOffset = 0, .yOffset = -8};
    }
    soft_ranges[0] = (glyph_array_t){.first = '0', .number = 3, .glyphs = soft_glyphs};
    soft_font      = (dgx_font_t){.glyph_ranges = soft_ranges, .yAdvance = 10, .yOffsetLowest = -8, .yBottomMax = 2, .xWidest = 7,
                                  .xWidthAverage = 7, .f_type = DGX_FONT_BITMAP_DEPTH4, .xRightMax = 7, .number_of_ranges = 1};
}

/* the rectangle of the upright text, from the start of its base line, as it lies on the screen when the text is turned */
static void turned_rect(dgx_output_orientation_t o, int ox, int oy, int w, int h, int *sx, int *sy, int *sw, int *sh)
{
    if (dgx_output_orientation_swap_xy(o)) {
        int t = ox;
        ox = oy, oy = t, t = w, w = h, h = t;
    }
    if (dgx_output_orientation_xdir(o) == DgxScreenRightLeft) ox = -ox - w;
    if (dgx_output_orientation_ydir(o) == DgxScreenBottomTop) oy = -oy - h;
    *sx = ox, *sy = oy, *sw = w, *sh = h;
}

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static void test_fields(void)
{
    static const char *const texts[] = {"0", "12", "0120", "21021021021", ""};
    static const char *const words[] = {"1", "42", "-7.5", "1234567890", ""};
    dgx_screen_t *a = dgx_vscreen_init(W, H, 16, DgxScreenRGB), *b = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    panel_t      *panel = panel_make(sizeof(((panel_t *)0)->buffer));
    for (int round = 0; round < 4000; ++round) {
        const bool        soft  = round % 2;
        dgx_font_t       *font  = soft ? &soft_font : TerminusTTFMedium12();
        const char       *text  = soft ? texts[rnd(0, 4)] : words[rnd(0, 4)];
        const int         scale = rnd(1, 3), field = rnd(1, 70);
        const dgx_output_orientation_t turn  = (dgx_output_orientation_t)rnd(0, 7);
        const dgx_font_align_t         align = (dgx_font_align_t)rnd(0, 2);
        const int                      x = rnd(-10, W + 10), y = rnd(-10, H + 10);
        const uint32_t color = (uint32_t)rnd(0, 0xffff), bg = (uint32_t)rnd(0, 0xffff);
        dgx_font_context_t ctx = {.font = font, .color = color, .bg = bg, .orientation = turn, .scale = scale, .field = field, .align = align};

        /* on a screen that is read: the field filled, the text set in it and cut by it */
        ctx.scr = a;
        paint(a, 0x0f0f);
        CHECK(dgx_font_draw(&ctx, x, y, text) == field);
        int fx, fy, fw, fh;
        turned_rect(turn, 0, font->yOffsetLowest * scale, field, (font->yBottomMax - font->yOffsetLowest) * scale, &fx, &fy, &fw, &fh);
        fx += x, fy += y;
        int advance = 0;
        for (const char *c = text; *c; ++c) {
            int16_t step;
            dgx_font_find_glyph((uint32_t)(unsigned char)*c, font, &step);
            advance += step * scale;
        }
        const int room  = field - advance;
        const int start = floor_div(align == DGX_FONT_ALIGN_RIGHT ? room : align == DGX_FONT_ALIGN_CENTER ? room / 2 : 0, scale) * scale;
        const bool swap_xy = dgx_output_orientation_swap_xy(turn);
        const int  step    = swap_xy ? dgx_output_orientation_ydir(turn) : dgx_output_orientation_xdir(turn);
        paint(b, 0x0f0f);
        dgx_screen_clip(b, fx, fy, fw, fh);
        dgx_fill_rectangle(b, fx, fy, fw, fh, bg);
        dgx_font_context_t over = ctx;
        over.scr = b, over.field = 0, over.flags = DGX_FONT_BG;
        dgx_font_draw(&over, swap_xy ? x : x + start * step, swap_xy ? y + start * step : y, text);
        dgx_screen_unclip(b);
        const int wrong = differ(a, b);
        if (wrong) fprintf(stderr, "field %d of \"%s\", %s, align %d, turn %d, scale %d: %d pixels wrong\n", field, text, soft ? "soft" : "terminus", align, turn,
                           scale, wrong);
        CHECK(wrong == 0);
        /* the clip is as it was */
        int cx, cy, cw, ch;
        dgx_screen_clip_get(a, &cx, &cy, &cw, &ch);
        CHECK(cx == 0 && cy == 0 && cw == W && ch == H);

        /* on a display behind a bus that cannot be read: the same pixels, every one of the field written once */
        dgx_screen_t *lcd = &panel->sbus.scr;
        ctx.scr           = lcd;
        panel_paint(panel, 0x0f0f);
        CHECK(dgx_font_draw(&ctx, x, y, text) == field);
        CHECK(differ_panel(panel, a) == 0);
        const int l = DGX_MAX(fx, 0), t = DGX_MAX(fy, 0), r = DGX_MIN(fx + fw, W), bt = DGX_MIN(fy + fh, H);
        if (r > l && bt > t) CHECK(once_in(panel, l, t, r, bt));

        /* a character in a field is a text of one character */
        if (text[0]) {
            ctx.scr = b;
            paint(b, 0x0f0f);
            CHECK(dgx_font_draw_char(&ctx, x, y, (uint32_t)(unsigned char)text[0]) == field);
            char one[2] = {text[0], 0};
            ctx.scr     = a;
            paint(a, 0x0f0f);
            dgx_font_draw(&ctx, x, y, one);
            CHECK(differ(a, b) == 0);
        }
    }
    /* a number drawn over a longer one in the same field: nothing of the longer one is left */
    const dgx_font_context_t counter = {.scr = a, .font = TerminusTTFMedium12(), .color = 0xffff, .bg = 0x0010, .field = 60, .align = DGX_FONT_ALIGN_RIGHT};
    dgx_font_context_t       fresh   = counter;
    fresh.scr                        = b;
    paint(a, 0), paint(b, 0);
    dgx_font_draw(&counter, 20, 30, "1234567");
    dgx_font_draw(&counter, 20, 30, "8");
    dgx_font_draw(&fresh, 20, 30, "8");
    CHECK(differ(a, b) == 0);
    dgx_screen_destroy(&a), dgx_screen_destroy(&b);
    free(panel);
}

int main(void)
{
    make_soft_font();
    test_opaque_pictures();
    test_fields();
    CHECK_DONE();
}
