/*
 * The pictures of the README, drawn by DGX itself on a host: the banner and the pictures of some recipes.
 *
 *   banner <variant> <out.pam> [dark|light]
 *
 * Variants: a - the banner for a light page, b - the banner for a dark page, c - the pen alone;
 * f - fonts, g - gauges, h - a handwritten text, p - figures, each for a dark page or, with `light`, a light one.
 *
 * A picture is drawn twice the size it is shown at, on a 16-bit virtual screen, and written as a PAM file
 * with transparency: it has no background, the page shows through it.
 * To build it and make the two banners, from the root of the repository:
 *
 *   cc -O2 -std=gnu11 -Itest/host/stubs -Iinclude -Isrc -include test/host/stubs/esp_log.h docs/images/banner.c \
 *      src/matrix.c src/morph.c src/morph_sources.c src/morph_render.c src/font.c src/pixels.c src/ellipse.c src/scroll.c \
 *      src/hw_font.c src/hw_morph.c src/font_morph.c src/bitmap.c src/draw.c src/bezier.c src/texture.c src/orientation.c \
 *      src/drivers/vscreen.c src/bw_screen.c src/dgx_bus_optimized_funcs.c src/dgx_spi_area_protocol.c src/gauge.c \
 *      src/fonts/?*.c -lm -o /tmp/banner
 *   /tmp/banner b /tmp/x.pam && convert /tmp/x.pam docs/images/banner-dark.png
 *   /tmp/banner a /tmp/x.pam && convert /tmp/x.pam docs/images/banner-light.png
 *   for v in f:fonts g:gauge h:handwriting p:figures; do for t in dark light; do
 *       /tmp/banner ${v%%:*} /tmp/x.pam $t && convert /tmp/x.pam docs/images/${v#*:}-$t.png; done; done
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_gauge.h"
#include "dgx_hw_font.h"
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_morph_sources.h"
#include "drivers/vscreen.h"
#include "fonts/ArialRegular12.h"
#include "fonts/DroidSansRegular9.h"
#include "fonts/IBMCGALight8x16Light8x1616.h"
#include "fonts/IBMPlexMonoMedium28.h"
#include "fonts/NotoMonoRegular28.h"
#include "fonts/TerminusTTFMedium12.h"
#include "fonts/VerdanaRegular32.h"
#include "fonts/WeatherIconsRegular27.h"
#include "fonts/font0ss.h"

/* the size of the picture: twice what it is shown at */
static int W = 1600, H = 400;

static dgx_screen_t *canvas, *ink, *lights;

/*
 * A picture has no background of its own: the page shows through. Its figures are drawn over a color nothing
 * else has, its ink is kept as amounts on a layer of its own and its glow as brightness on another, and the
 * three are put together with their transparency when the file is written.
 */
#define NOTHING 0xF81Fu
static uint8_t ink_color[3], glow_color[3] = {70, 215, 255};

static uint32_t rgb(int r, int g, int b)
{
    return dgx_rgb_to_16((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void ink_clear(void)
{
    dgx_fill_rectangle(ink, 0, 0, W, H, 0);
}

/* a handwritten text as large as fits a box, its middle in the middle of the box; returns the scale */
static float written(const char *text, int x, int y, int w, int h, int pen, float most)
{
    dgx_font_t *font = font0ss();
    int         left, top, right, bottom;
    if (!dgx_hw_text_box(font, text, &left, &top, &right, &bottom)) return 0;
    const float cw = right - left + 1, ch = bottom - top + 1;
    float       scale = (w - pen) / cw < (h - pen) / ch ? (w - pen) / cw : (h - pen) / ch;
    if (most > 0 && scale > most) scale = most;
    const int tx = (int)lroundf(x + (w - cw * scale) / 2 - left * scale), ty = (int)lroundf(y + (h - ch * scale) / 2 - top * scale);
    dgx_hw_draw_text(ink, tx, ty, font, text, scale, DGX_PEN_SOFT | pen, 255, true);
    return scale;
}

static uint32_t gauge_color(int value)
{
    return value < 60 ? rgb(40, 170, 90) : value < 85 ? rgb(240, 190, 40) : rgb(220, 60, 40);
}

/* the figures of the recipes in a row: a pie, the sign of Wi-Fi, a gauge, arcs by pens */
static void figures(int x, int y, int r, uint32_t line, uint32_t rest, bool dark)
{
    const uint32_t red = rgb(215, 70, 55), green = rgb(45, 160, 95), blue = rgb(50, 110, 225), yellow = rgb(240, 195, 50);
    /* a pie */
    dgx_solid_sector(canvas, x, y, r, r, 0, 100, red);
    dgx_solid_sector(canvas, x, y, r, r, 100, 190, green);
    dgx_solid_sector(canvas, x, y, r, r, 190, 300, blue);
    dgx_solid_sector(canvas, x, y, r, r, 300, 360, yellow);
    for (int k = 0; k < 3; ++k) dgx_draw_ellipse(canvas, x, y, r + k, r + k, line);
    /* a gauge */
    static dgx_gauge_t gauge;
    dgx_gauge_init(&gauge, canvas, x + r * 6, y + r / 6, r * 2 / 3, r / 3, 225.0f, 270, 0, 100, rest, gauge_color);
    dgx_gauge_set_value(&gauge, 72);
    /* arcs by pens */
    const int ax = x + r * 9 + r / 2;
    dgx_draw_arc(canvas, ax, y, r, r, 0, 270, r / 20 + 1, line);
    dgx_draw_arc(canvas, ax, y, r * 3 / 4, r * 3 / 4, 45, 315, r / 9, red);
    dgx_draw_arc(canvas, ax, y, r / 2, r / 2, 200, 520, r / 7, dark ? rgb(90, 200, 255) : blue);
    dgx_draw_arc(canvas, ax, y, r / 5, r / 5, 0, 360, r / 14 + 1, green);
}

/* a word in glowing dots, the dots of a bitmap font; returns its width */
static int glowing(const char *text, int x, int y, int cell, const uint32_t *colors)
{
    dgx_font_t *font = TerminusTTFMedium12();
    const int   radius = cell / 2 + cell / 4;
    int         at = x;
    for (const char *p = text; *p; ++p) {
        dgx_bit_matrix_t *from = dgx_morph_glyph_matrix(font, (uint32_t)*p), *to = dgx_morph_glyph_matrix(font, (uint32_t)*p);
        dgx_morph_t      *morph = dgx_morph_create(from, to, dgx_morph_sources_cells, NULL);
        const int         w = from->width * cell + 2 * radius, h = from->height * cell + 2 * radius;
        dgx_morph_glow_t *glow = dgx_morph_glow_create(w, h, cell, 16);
        dgx_morph_glow_set_lut(glow, colors);
        dgx_morph_draw(morph, 1.0f, radius, radius, cell, true, dgx_morph_glow_dot, glow);
        dgx_morph_glow_present(glow, 1.0f, lights, at, y);
        at += from->width * cell + cell;
        dgx_morph_glow_destroy(&glow);
        dgx_morph_destroy(&morph);
        dgx_matrix_destroy(&from);
        dgx_matrix_destroy(&to);
    }
    return at - x;
}

static void text(dgx_font_t *font, int x, int y, int scale, uint32_t color, const char *str)
{
    dgx_font_string_utf8_screen(canvas, (int16_t)x, (int16_t)y, str, color, DgxOutputNormal, scale, font, NULL, NULL);
}

/* the pictures of the recipes; `dark` is the page they are for */
static void recipe(char variant, bool dark)
{
    /* the colors of text and lines, of what is of less weight, and two to tell things apart */
    const uint32_t line = dark ? rgb(230, 235, 245) : rgb(24, 36, 120), dim = dark ? rgb(140, 146, 160) : rgb(110, 114, 125);
    const uint32_t warm = dark ? rgb(255, 190, 70) : rgb(190, 105, 0), cold = dark ? rgb(110, 215, 255) : rgb(20, 105, 190);
    const uint32_t rest = dark ? rgb(84, 90, 104) : rgb(190, 192, 198);
    dgx_fill_rectangle(canvas, 0, 0, W, H, NOTHING);
    ink_clear();
    ink_color[0] = dark ? 230 : 24, ink_color[1] = dark ? 235 : 36, ink_color[2] = dark ? 245 : 120;
    switch (variant) {
    case 'f': { /* the fonts that come with the library, each naming itself */
        int y = 44;
        text(TerminusTTFMedium12(), 24, y, 2, line, "TerminusTTFMedium12  Привет, мир! Grüße"), y += 50;
        text(ArialRegular12(), 24, y, 2, line, "ArialRegular12  Hello, world! ¿Qué tal?"), y += 50;
        text(DroidSansRegular9(), 24, y, 2, dim, "DroidSansRegular9  the quick brown fox jumps over the lazy dog"), y += 44;
        text(IBMCGALight8x16Light8x1616(), 24, y + 6, 2, cold, "IBMCGALight8x16  C:\\> dir /w"), y += 78;
        text(VerdanaRegular32(), 24, y, 2, warm, "Verdana 32"), text(VerdanaRegular32(), 560, y, 1, warm, "VerdanaRegular32"), y += 84;
        text(IBMPlexMonoMedium28(), 24, y, 2, line, "12:34"), text(IBMPlexMonoMedium28(), 360, y, 1, line, "IBMPlexMonoMedium28"),
            text(NotoMonoRegular28(), 760, y, 1, dim, "NotoMonoRegular28"), y += 96;
        /* the weather symbols a clock shows */
        static const char *weather = "\xef\x80\x8d \xef\x80\x82 \xef\x80\x93 \xef\x80\x99 \xef\x80\x9e \xef\x80\x9b \xef\x80\x94";
        text(WeatherIconsRegular27(), 24, y, 2, cold, weather), text(TerminusTTFMedium12(), 760, y - 16, 2, dim, "WeatherIconsRegular27");
        break;
    }
    case 'g': { /* a gauge at three values */
        static dgx_gauge_t gauge[3];
        static const int   values[3] = {18, 64, 93};
        for (int i = 0; i < 3; ++i) {
            const int cx = W / 6 + i * W / 3, cy = H / 2 + 16;
            char      label[8];
            dgx_gauge_init(&gauge[i], canvas, cx, cy, 96, 40, 225.0f, 270, 0, 100, rest, gauge_color);
            dgx_gauge_set_value(&gauge[i], values[i]);
            snprintf(label, sizeof(label), "%d", values[i]);
            int16_t   yc, hh;
            const int w = dgx_font_string_bounds(label, VerdanaRegular32(), &yc, &hh) * 2;
            text(VerdanaRegular32(), cx - w / 2, cy + 22, 2, line, label);
        }
        break;
    }
    case 'h': { /* a text written by hand: finished, and the pen half way through it */
        dgx_font_t *font = font0ss();
        const char *words = "Hello, World";
        int         left, top, right, bottom;
        dgx_hw_text_box(font, words, &left, &top, &right, &bottom);
        const int   pen = 9, row = H / 2;
        const float cw = right - left + 1, ch = bottom - top + 1;
        const float scale = (W - 80 - pen) / cw < (row - 36 - pen) / ch ? (W - 80 - pen) / cw : (row - 36 - pen) / ch;
        const int   x = (int)lroundf((W - cw * scale) / 2 - left * scale), y = (int)lroundf((row - ch * scale) / 2 - top * scale);
        dgx_hw_draw_text(ink, x, y, font, words, scale, DGX_PEN_SOFT | pen, 255, true);
        dgx_hw_pace_t    pace = dgx_hw_pace(font, 6);
        dgx_hw_writing_t writing;
        dgx_hw_writing_begin(&writing, ink, x, y + row, font, words, scale, DGX_PEN_SOFT | pen, 255, true, &pace);
        dgx_hw_writing_draw_to(&writing, 0.56f * dgx_hw_text_effort(font, words, true, &pace));
        break;
    }
    default: { /* figures: a pie, sectors, arcs by pens, the sign of Wi-Fi */
        const uint32_t red = rgb(215, 70, 55), green = rgb(45, 160, 95), blue = rgb(50, 110, 225), yellow = rgb(240, 195, 50);
        const int      y = H / 2, r = 110;
        int            x = 150;
        dgx_solid_sector(canvas, x, y, r, r, 0, 100, red);
        dgx_solid_sector(canvas, x, y, r, r, 100, 190, green);
        dgx_solid_sector(canvas, x, y, r, r, 190, 300, blue);
        dgx_solid_sector(canvas, x, y, r, r, 300, 360, yellow);
        for (int k = 0; k < 3; ++k) dgx_draw_ellipse(canvas, x, y, r + k, r + k, line);
        x += 300; /* a sector over a half turn, and one taken out of it */
        dgx_solid_sector(canvas, x, y, r, r, 30, 330, yellow);
        dgx_solid_sector(canvas, x + 26, y, r, r, -30, 30, red);
        x += 330; /* sectors and the outline of an ellipse */
        dgx_solid_sector(canvas, x, y, 150, 80, 20, 160, green);
        dgx_solid_sector(canvas, x, y, 150, 80, 200, 340, blue);
        for (int k = 0; k < 3; ++k) dgx_draw_ellipse(canvas, x, y, 150 + k, 80 + k, line);
        x += 330; /* arcs by hard pens */
        dgx_draw_arc(canvas, x, y, 110, 110, 0, 270, 4, line);
        dgx_draw_arc(canvas, x, y, 84, 84, 45, 315, 10, red);
        dgx_draw_arc(canvas, x, y, 54, 54, 200, 520, 16, blue);
        dgx_draw_arc(canvas, x, y, 24, 24, 0, 360, 6, green);
        x += 300; /* the sign of Wi-Fi by a soft pen */
        dgx_solid_circle(ink, x, y + 80, 16, 255);
        for (int i = 1; i <= 3; ++i) dgx_draw_arc(ink, x, y + 80, i * 52, i * 52, 45, 135, DGX_PEN_SOFT | 20, 255);
        break;
    }
    }
}

int main(int argc, char **argv)
{
    const char variant = argc > 1 ? argv[1][0] : 'a';
    if (variant == 'f') W = 1400, H = 470;
    if (variant == 'g') W = 1200, H = 420;
    if (variant == 'h') W = 1400, H = 420;
    if (variant == 'p') W = 1560, H = 300;
    canvas = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    ink = dgx_vscreen_init(W, H, 8, DgxScreenRGB);
    lights = dgx_vscreen_init(W, H, 16, DgxScreenRGB);
    const bool banner = variant == 'a' || variant == 'b' || variant == 'c';
    if (!banner) recipe(variant, argc < 4 || argv[3][0] != 'l');
    const char *words = "display graphics for microcontrollers";
    if (!banner) {
        /* drawn above */
    } else if (variant == 'a') {
        /* for a light page: the name by a pen, the figures in a row, the words under them */
        dgx_fill_rectangle(canvas, 0, 0, W, H, NOTHING);
        figures(742, 150, 76, rgb(24, 36, 120), rgb(190, 192, 198), false);
        ink_clear();
        written("DGX", 30, 10, 560, 380, 22, 0);
        const int wx = 742 + 76 * 3, wy = 150 + 76 * 3 / 5, step = 76 * 2 / 5, pen = 76 / 6;
        dgx_solid_circle(ink, wx, wy, pen, 255);
        for (int i = 1; i <= 3; ++i) dgx_draw_arc(ink, wx, wy, i * step, i * step, 45, 135, DGX_PEN_SOFT | pen, 255);
        written(words, 660, 270, 900, 110, 6, 0);
        ink_color[0] = 24, ink_color[1] = 36, ink_color[2] = 120;
    } else if (variant == 'b') {
        /* for a dark page: the name in glowing dots, the words by a light pen, the figures */
        dgx_fill_rectangle(canvas, 0, 0, W, H, NOTHING);
        uint32_t brightness[256];
        for (int i = 0; i < 256; ++i) brightness[i] = rgb(i, i, i);
        glowing("DGX", 40, 60, 22, brightness);
        figures(742, 150, 76, rgb(225, 230, 240), rgb(84, 90, 104), true);
        ink_clear();
        const int wx = 742 + 76 * 3, wy = 150 + 76 * 3 / 5, step = 76 * 2 / 5, pen = 76 / 6;
        dgx_solid_circle(ink, wx, wy, pen, 255);
        for (int i = 1; i <= 3; ++i) dgx_draw_arc(ink, wx, wy, i * step, i * step, 45, 135, DGX_PEN_SOFT | pen, 255);
        written(words, 660, 270, 900, 110, 6, 0);
        ink_color[0] = 230, ink_color[1] = 235, ink_color[2] = 245;
    } else {
        /* the pen alone: the name and the words */
        dgx_fill_rectangle(canvas, 0, 0, W, H, NOTHING);
        ink_clear();
        written("DGX", 60, 10, 620, 380, 24, 0);
        written(words, 720, 120, 840, 160, 7, 0);
        ink_color[0] = 24, ink_color[1] = 36, ink_color[2] = 120;
    }
    /* a PAM file: red, green, blue and how much of the page each pixel covers */
    FILE *out = fopen(argc > 2 ? argv[2] : "banner.pam", "wb");
    fprintf(out, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", W, H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const uint32_t c = dgx_get_pixel(canvas, x, y);
            float          r = ((c >> 11) & 31) * 255 / 31.0f, g = ((c >> 5) & 63) * 255 / 63.0f, b = (c & 31) * 255 / 31.0f, a = 1;
            {
                if (c == NOTHING) r = g = b = a = 0;
                /* the glow over the figures: as bright, so much of it; the brightest of it turns white */
                const float lit = ((dgx_get_pixel(lights, x, y) >> 5) & 63) / 63.0f, hot = lit > 0.7f ? (lit - 0.7f) / 0.3f : 0;
                const float glow[3] = {glow_color[0] + (255 - glow_color[0]) * hot, glow_color[1] + (255 - glow_color[1]) * hot, glow_color[2]};
                /* then the ink, by its amount */
                const float amount = dgx_get_pixel(ink, x, y) / 255.0f;
                const float layers[2][4] = {{glow[0], glow[1], glow[2], lit}, {ink_color[0], ink_color[1], ink_color[2], amount}};
                for (int i = 0; i < 2; ++i) {
                    const float over = layers[i][3], all = over + a * (1 - over);
                    if (all > 0) {
                        r = (layers[i][0] * over + r * a * (1 - over)) / all;
                        g = (layers[i][1] * over + g * a * (1 - over)) / all;
                        b = (layers[i][2] * over + b * a * (1 - over)) / all;
                    }
                    a = all;
                }
            }
            fputc((int)(r + 0.5f), out), fputc((int)(g + 0.5f), out), fputc((int)(b + 0.5f), out), fputc((int)(a * 255 + 0.5f), out);
        }
    }
    fclose(out);
    return 0;
}
