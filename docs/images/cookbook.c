/*
 * The pictures of the cookbook, docs/cookbook-en.md, drawn on a host by the code of its recipes, which this file
 * holds as the page gives it.
 *
 *   cookbook <picture> <out.ppm>
 *
 * Pictures: align - text set at a point and in a box, value - a value over a gradient, wrap - text wrapped to a
 * width, button - a button and the same one pressed, bars - a progress bar and batteries. Each is a screen of
 * 16 bits a pixel, written at twice its size, a pixel of the screen as 2x2. The value takes DejaVu Sans at
 * 32 px in 4 bits a pixel with the degree sign, made by font2c first. From the root of the repository:
 *
 *   cc font2c/font2c.c -o /tmp/font2c $(pkg-config --cflags --libs freetype2) && mkdir -p /tmp/dgx-cookbook
 *   F=$(fc-match -f '%{file}' 'DejaVu Sans:style=Book')
 *   (cd /tmp/dgx-cookbook && /tmp/font2c -n temperature -b 4 $F 32 0x20 0x7e 0xb0 0xb0)
 *   cc -O2 -std=gnu11 -Ihost/stubs -Iinclude -I/tmp/dgx-cookbook -include host/stubs/esp_log.h docs/images/cookbook.c \
 *      src/screen/*.c src/draw/*.c src/vscreen/*.c src/text/*.c src/morph/*.c src/fonts/?*.c /tmp/dgx-cookbook/*.c \
 *      -lm -o /tmp/cookbook
 *   for p in align value wrap button bars; do /tmp/cookbook $p /tmp/x.ppm && convert /tmp/x.ppm docs/images/cookbook-$p.png; done
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_vscreen.h"
#include "fonts/LiberationSansRegular12.h"
#include "fonts/TerminusTTFMedium12.h"
#include "temperature.h"

#define dgx_font_temperature temperature

/* ---- text at a point ---- */

/* The text ends at x. */
int text_right(const dgx_font_context_t *ctx, int x, int y, const char *str)
{
    return dgx_font_draw(ctx, x - dgx_font_bounds(ctx, str, NULL, NULL), y, str);
}

/* The middle of the text is at x. */
int text_center(const dgx_font_context_t *ctx, int x, int y, const char *str)
{
    return dgx_font_draw(ctx, x - dgx_font_bounds(ctx, str, NULL, NULL) / 2, y, str);
}

/* The text in the middle of a box, both ways, by its own letters. */
void text_in_box(const dgx_font_context_t *ctx, int x, int y, int w, int h, const char *str)
{
    int       top, height;
    const int width = dgx_font_bounds(ctx, str, &top, &height);
    dgx_font_draw(ctx, x + (w - width) / 2, y + (h - height) / 2 - top, str);
}

/* The base line that puts the cell of the font in the middle of a box of height h at y. */
int base_line_in_box(const dgx_font_context_t *ctx, int y, int h)
{
    const int scale = ctx->scale ? ctx->scale : 1;
    const int top = ctx->font->y_offset_lowest * scale, bottom = ctx->font->y_bottom_max * scale;
    return y + (h - (bottom - top)) / 2 - top;
}

/* ---- wrapped text ---- */

/*
 * Draws str in lines no wider than w, breaking at spaces and at '\n'; the first base line is y.
 * Returns the base line after the last line. A word wider than w stands on a line of its own.
 */
int text_wrapped(const dgx_font_context_t *ctx, int x, int y, int w, const char *str)
{
    const int step = ctx->font->y_advance * (ctx->scale ? ctx->scale : 1);
    char      line[128];
    for (;;) {
        while (*str == ' ') ++str;
        if (*str == 0) return y;
        size_t end = 0; /* the line is str[0..end) */
        while (str[end] && str[end] != '\n') {
            size_t next = end;
            while (str[next] == ' ') ++next;
            if (str[next] == 0 || str[next] == '\n') break; /* only spaces to the end of the line */
            while (str[next] && str[next] != ' ' && str[next] != '\n') ++next;
            if (next >= sizeof line) break;
            memcpy(line, str, next);
            line[next] = 0;
            if (end > 0 && dgx_font_bounds(ctx, line, NULL, NULL) > w) break;
            end = next;
        }
        if (end == 0 && *str != '\n') { /* a word longer than the buffer: as much of it as fits */
            end = sizeof line - 1;
            while ((str[end] & 0xC0) == 0x80) --end; /* not in the middle of a character */
        }
        memcpy(line, str, end);
        line[end] = 0;
        dgx_font_draw(ctx, x, y, line);
        y += step;
        str += end;
        while (*str == ' ') ++str;
        if (*str == '\n') ++str;
    }
}

/* ---- a button ---- */

void button(dgx_screen_t *scr, const dgx_font_t *font, int x, int y, int w, int h, const char *label, bool pressed)
{
    const uint32_t face = pressed ? DGX_ORANGE(dgx_rgb_to_16) : DGX_MIDNIGHTBLUE(dgx_rgb_to_16);
    const uint32_t ink  = pressed ? DGX_BLACK(dgx_rgb_to_16) : DGX_WHITE(dgx_rgb_to_16);
    dgx_fill_round_rectangle(scr, x, y, w, h, h / 4, face);
    dgx_draw_round_rectangle(scr, x, y, w, h, h / 4, ink);
    const dgx_font_context_t text = {.scr = scr, .font = font, .color = ink, .bg = face, .flags = DGX_FONT_BG};
    text_in_box(&text, x, y, w, h, label);
}

bool button_hit(int x, int y, int w, int h, int touch_x, int touch_y)
{
    return touch_x >= x && touch_x < x + w && touch_y >= y && touch_y < y + h;
}

/* ---- a progress bar ---- */

/* A bar of w x h at x, y, done to `percent`: every pixel written once, so it does not blink. */
void progress_bar(dgx_screen_t *scr, int x, int y, int w, int h, int percent, uint32_t done, uint32_t rest)
{
    const int inner = w - 4, filled = inner * percent / 100;
    dgx_draw_rectangle(scr, x, y, w, h, done);
    dgx_draw_rectangle(scr, x + 1, y + 1, w - 2, h - 2, rest);
    dgx_fill_rectangle(scr, x + 2, y + 2, filled, h - 4, done);
    dgx_fill_rectangle(scr, x + 2 + filled, y + 2, inner - filled, h - 4, rest);
}

/* ---- a battery ---- */

/* A battery of w x h at x, y, its charge `level` 0 to 100, on a background of the color `back`. */
void battery(dgx_screen_t *scr, int x, int y, int w, int h, int level, uint32_t back)
{
    const uint32_t frame  = DGX_WHITE(dgx_rgb_to_16);
    const uint32_t charge = level < 20   ? DGX_RED(dgx_rgb_to_16)
                            : level < 50 ? DGX_YELLOW(dgx_rgb_to_16)
                                         : DGX_LIMEGREEN(dgx_rgb_to_16);
    const int      tip = w / 10 > 2 ? w / 10 : 2, body = w - tip;
    const int      inner = body - 6, filled = inner * level / 100;
    dgx_draw_round_rectangle(scr, x, y, body, h, 3, frame);
    dgx_fill_rectangle(scr, x + body, y + h / 3, tip, h - 2 * (h / 3), frame);
    dgx_fill_rectangle(scr, x + 3, y + 3, filled, h - 6, charge);
    dgx_fill_rectangle(scr, x + 3 + filled, y + 3, inner - filled, h - 6, back);
}

/* ---- the pictures ---- */

static int width, height;
#define LCD_HEIGHT height

/* The background of the screen, a gradient from navy at its top to copper at its bottom, as it lies at x, y. */
static void draw_background(dgx_screen_t *piece, int x, int y)
{
    const dgx_round_rectangle_t r = {
        .w     = piece->width * DGX_SUBPIXEL,
        .h     = piece->height * DGX_SUBPIXEL,
        .paint = {.color  = DGX_NAVY(dgx_rgb_to_16),
                  .color2 = DGX_COPPER(dgx_rgb_to_16),
                  .gy     = {-y * DGX_SUBPIXEL, (LCD_HEIGHT - y) * DGX_SUBPIXEL},
                  .flags  = DGX_PAINT_LINEAR}};
    (void)x;
    dgx_fill_round_rectangle_with(piece, &r);
}

static dgx_screen_t *piece; /* dgx_vscreen_init(180, 50, 16, DgxScreenRGB), made once */

/* The value at x, y of the display, its right end at the right end of the piece, with a shadow. */
static void show_temperature(dgx_screen_t *lcd, int x, int y, const char *text)
{
    draw_background(piece, x, y);
    const dgx_font_context_t shadow = {.scr = piece, .font = &dgx_font_temperature, .color = DGX_BLACK(dgx_rgb_to_16)};
    const dgx_font_context_t ink    = {.scr = piece, .font = &dgx_font_temperature, .color = DGX_WHITE(dgx_rgb_to_16)};
    const int                base   = base_line_in_box(&ink, 0, piece->height);
    text_right(&shadow, piece->width, base + 2, text);
    text_right(&ink, piece->width - 2, base, text);
    dgx_vscreen_to_screen(lcd, x, y, piece, 0);
}

static dgx_screen_t *picture(const char *which)
{
    const uint32_t white = DGX_WHITE(dgx_rgb_to_16), grey = DGX_DIMGREY(dgx_rgb_to_16), black = DGX_BLACK(dgx_rgb_to_16);
    dgx_screen_t  *scr   = NULL;
    if (!strcmp(which, "align")) {
        scr = dgx_vscreen_init(width = 320, height = 230, 16, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, width, height, black);
        const dgx_font_context_t ctx = {.scr = scr, .font = &LiberationSansRegular12, .color = white, .scale = 2};
        dgx_draw_line(scr, 20, 10, 20, 120, grey);
        dgx_draw_line(scr, 160, 10, 160, 120, grey);
        dgx_draw_line(scr, 300, 10, 300, 120, grey);
        dgx_font_draw(&ctx, 20, 40, "Left");
        text_center(&ctx, 160, 75, "Center");
        text_right(&ctx, 300, 110, "Right");
        dgx_draw_rectangle(scr, 60, 145, 200, 70, grey);
        text_in_box(&ctx, 60, 145, 200, 70, "In a box");
    } else if (!strcmp(which, "value")) {
        scr   = dgx_vscreen_init(width = 320, height = 110, 16, DgxScreenRGB);
        piece = dgx_vscreen_init(180, 50, 16, DgxScreenRGB);
        draw_background(scr, 0, 0);
        show_temperature(scr, 70, 30, "23.5°C");
        dgx_screen_destroy(&piece);
    } else if (!strcmp(which, "wrap")) {
        scr = dgx_vscreen_init(width = 230, height = 104, 16, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, width, height, black);
        const dgx_font_context_t ctx = {.scr = scr, .font = &LiberationSansRegular12, .color = white};
        dgx_draw_rectangle(scr, 9, 9, 182, 86, grey);
        text_wrapped(&ctx, 12, 12 - ctx.font->y_offset_lowest, 176,
                     "Mostly sunny early, then clouds increasing in the afternoon. High 54F. Winds NW at 10 to 15 "
                     "mph.\nTonight: partly cloudy, low 38F.");
    } else if (!strcmp(which, "button")) {
        scr = dgx_vscreen_init(width = 320, height = 90, 16, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, width, height, black);
        button(scr, &LiberationSansRegular12, 30, 20, 120, 50, "Start", false);
        button(scr, &LiberationSansRegular12, 170, 20, 120, 50, "Start", true);
    } else if (!strcmp(which, "bars")) {
        scr = dgx_vscreen_init(width = 320, height = 120, 16, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, width, height, black);
        progress_bar(scr, 20, 16, 220, 24, 70, DGX_DEEPSKYBLUE(dgx_rgb_to_16), black);
        const dgx_font_context_t percent = {.scr   = scr,
                                            .font  = &TerminusTTFMedium12,
                                            .color = white,
                                            .bg    = black,
                                            .field = 50,
                                            .align = DGX_FONT_ALIGN_RIGHT};
        dgx_font_draw(&percent, 250, 33, "70%");
        battery(scr, 30, 68, 70, 32, 90, black);
        battery(scr, 125, 68, 70, 32, 45, black);
        battery(scr, 220, 68, 70, 32, 10, black);
    }
    return scr;
}

int main(int argc, char **argv)
{
    dgx_screen_t *scr = picture(argc > 1 ? argv[1] : "align");
    if (!scr) return 1;
    uint32_t *px = malloc(sizeof(uint32_t) * width * height);
    if (!px || !dgx_vscreen_to_argb8888(scr, 0, 0, width, height, NULL, px, width)) return 1;
    FILE *out = fopen(argc > 2 ? argv[2] : "cookbook.ppm", "wb");
    if (!out) return 1;
    fprintf(out, "P6\n%d %d\n255\n", 2 * width, 2 * height);
    for (int y = 0; y < 2 * height; ++y)
        for (int x = 0; x < 2 * width; ++x) {
            const uint32_t c = px[y / 2 * width + x / 2];
            fputc(c >> 16 & 255, out), fputc(c >> 8 & 255, out), fputc(c & 255, out);
        }
    fclose(out);
    free(px);
    dgx_screen_destroy(&scr);
    return 0;
}
