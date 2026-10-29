/*
 * What each colour panel driver sends, recorded through a bus that takes it
 * all in order: commands and their parameters byte by byte, pixels by their
 * number of bits and a sum of their bytes, the waits of the screen for its
 * buffer, and the levels of the pins and the waits of the driver
 * (panel_record.h). Each driver is made at every depth it takes, in the
 * order of colours RGB with drawing and in BGR without, turned to the eight
 * orientations, and draws a pixel and a rectangle in each and a whole fill at
 * the end; its other calls are made once, and a depth it does not take is
 * asked for too.
 *
 * The record must be panel_streams.txt, made from the drivers as they were
 * before they were moved onto one core (ST77916 came after, on it): a byte more, less or in another
 * place fails, and the first line that differs is shown with the part it is
 * in. "test_panel_streams write" writes the file anew.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_screen.h"
#include "dgx_screen_with_bus.h"
#include "drivers/gc9a01.h"
#include "drivers/ili9341.h"
#include "drivers/nv3041a.h"
#include "drivers/ssd1351.h"
#include "drivers/st7735.h"
#include "drivers/st7789.h"
#include "drivers/st77916.h"

#define STREAMS "panel_streams.txt"

/* ---- the record ---- */

static char    *record;
static size_t   record_len, record_cap;
static long     pixel_bits; /* pixels in a row of transfers, one line once something else comes */
static uint64_t pixel_sum;

static void put(const char *fmt, ...)
{
    if (!record) record = malloc(record_cap = 1 << 16);
    for (;;) {
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(record + record_len, record_cap - record_len, fmt, args);
        va_end(args);
        if (n >= 0 && (size_t)n < record_cap - record_len) {
            record_len += (size_t)n;
            return;
        }
        record_cap *= 2;
        record = realloc(record, record_cap);
    }
}

static void pixels_end(void)
{
    if (pixel_bits) put("P %ld %016llx\n", pixel_bits, (unsigned long long)pixel_sum);
    pixel_bits = 0;
    pixel_sum  = 14695981039346656037u;
}

static void put_bytes(const char *what, const uint8_t *bytes, uint32_t bits)
{
    pixels_end();
    put("%s", what);
    for (uint32_t i = 0; i < (bits + 7u) / 8u; ++i) put(" %02x", bytes[i]);
    if (bits % 8u) put(" /%u", (unsigned)bits);
    put("\n");
}

esp_err_t panel_record_level(gpio_num_t pin, uint32_t level)
{
    pixels_end();
    put("G %d %u\n", (int)pin, (unsigned)level);
    return ESP_OK;
}

esp_err_t panel_record_mode(gpio_num_t pin, int mode)
{
    pixels_end();
    put("M %d %d\n", (int)pin, mode);
    return ESP_OK;
}

void panel_record_wait(uint32_t ms)
{
    pixels_end();
    put("W %u\n", (unsigned)ms);
}

/* ---- the bus ---- */

static uint8_t bus_buffer[4000];

static void bus_command(dgx_bus_protocols_t *bus, uint8_t cmd)
{
    (void)bus;
    pixels_end();
    put("C %02x\n", cmd);
}

static void bus_commands(dgx_bus_protocols_t *bus, const uint8_t *cmds, uint32_t bits)
{
    (void)bus;
    put_bytes("CC", cmds, bits);
}

static void bus_data(dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    put_bytes("D", data, bits);
}

static void bus_pixels(dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits)
{
    (void)bus;
    for (uint32_t i = 0; i < (bits + 7u) / 8u; ++i) pixel_sum = (pixel_sum ^ data[i]) * 1099511628211u;
    pixel_bits += (long)bits;
}

static void bus_sync(dgx_bus_protocols_t *bus)
{
    (void)bus;
    pixels_end();
    put("S\n");
}

static uint32_t bus_read(dgx_bus_protocols_t *bus, uint8_t *data, uint32_t bits)
{
    (void)bus;
    pixels_end();
    put("R %u\n", (unsigned)bits);
    memset(data, 0, (bits + 7u) / 8u);
    return 0;
}

static uint8_t *bus_memory(uint32_t len)
{
    return malloc(len);
}

static void bus_dispose(dgx_bus_protocols_t *bus)
{
    (void)bus;
}

static dgx_bus_protocols_t bus_of(enum dgx_bus_type type)
{
    dgx_bus_protocols_t bus;
    memset(&bus, 0, sizeof(bus));
    bus.bus_type         = type;
    bus.bus_name         = "record";
    bus.write_command    = bus_command;
    bus.write_commands   = bus_commands;
    bus.write_data       = bus_data;
    bus.read_data        = bus_read;
    bus.write_data_async = bus_pixels;
    bus.sync_write       = bus_sync;
    bus.memory_allocator = bus_memory;
    bus.dispose          = bus_dispose;
    bus.buffer           = bus_buffer;
    bus.buffer_len       = sizeof(bus_buffer);
    return bus;
}

/* ---- what the drivers are asked to do ---- */

typedef void (*orientation_func)(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/* the eight orientations; with drawing, a pixel and a rectangle in each (and a scroll of the panel) and a whole fill at the end */
static void turns(dgx_screen_t *s, orientation_func orientation, bool draw, bool scroll)
{
    for (int i = 0; i < 8; ++i) {
        const dgx_orientation_t x = i & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, y = i & 2 ? DgxScreenBottomTop : DgxScreenTopBottom;
        const bool              swap = i & 4;
        pixels_end();
        put("# turn %s %s%s\n", x == DgxScreenRightLeft ? "rl" : "lr", y == DgxScreenBottomTop ? "bt" : "tb", swap ? " swap" : "");
        orientation(s, x, y, swap);
        if (!draw) continue;
        dgx_set_pixel(s, 1, 2, 0x5a5);
        dgx_fill_rectangle(s, 3, 4, 10, 6, 0x0f0);
        if (scroll) dgx_scroll(s, 0, 0, s->width, s->height, 0, 8, 0x00f);
        s->wait_buffer(s);
    }
    if (draw) {
        pixels_end();
        put("# fill\n");
        dgx_fill_rectangle(s, 0, 0, s->width, s->height, 0xf00);
        s->wait_buffer(s);
    }
}

/* a part of the record: a driver at a depth in an order of colours, NULL when the driver refuses it */
static void part(const char *driver, const char *what, int bits, dgx_color_order_t order)
{
    pixels_end();
    put("## %s%s %d %s\n", driver, what, bits, order == DgxScreenRGB ? "rgb" : "bgr");
}

static void done(dgx_screen_t *s, bool expected)
{
    CHECK((s != NULL) == expected);
    if (s) dgx_screen_destroy(&s);
    pixels_end();
}

static void record_ili9341(void)
{
    static const int depths[] = {16, 18};
    for (size_t k = 0; k <= sizeof(depths) / sizeof(depths[0]); ++k) {
        const bool              bgr = k == sizeof(depths) / sizeof(depths[0]);
        const int               bits = bgr ? 16 : depths[k];
        const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
        dgx_bus_protocols_t     bus = bus_of(DGX_BUS_SPI);
        part("ili9341", "", bits, order);
        dgx_screen_t *s = dgx_ili9341_init(&bus, 4, 21, (uint8_t)bits, order);
        if (s) {
            turns(s, dgx_ili9341_orientation, !bgr, true);
            dgx_ili9341_backlight(s, false);
            dgx_ili9341_backlight(s, true);
        }
        done(s, true);
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_SPI);
    part("ili9341", " refused", 12, DgxScreenRGB);
    done(dgx_ili9341_init(&bus, 4, 21, 12, DgxScreenRGB), false);
}

static void record_st7789(void)
{
    static const int                 depths[]      = {12, 16, 18};
    static const st7789_resolution_t resolutions[] = {ST7789_RESOLUTION_240x240, ST7789_RESOLUTION_240x320, ST7789_RESOLUTION_320x240};
    static const char               *names[]       = {" 240x240", " 240x320", " 320x240"};
    for (size_t r = 0; r < 3; ++r) {
        for (size_t k = 0; k <= sizeof(depths) / sizeof(depths[0]); ++k) {
            const bool              bgr = k == sizeof(depths) / sizeof(depths[0]);
            const int               bits = bgr ? 16 : depths[k];
            const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
            dgx_bus_protocols_t     bus = bus_of(DGX_BUS_SPI);
            part("st7789", names[r], bits, order);
            dgx_screen_t *s = dgx_st7789_init(&bus, 4, (uint8_t)bits, order, resolutions[r]);
            if (s) turns(s, dgx_st7789_orientation, !bgr, false);
            done(s, true);
        }
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_SPI);
    part("st7789", " refused", 24, DgxScreenRGB);
    done(dgx_st7789_init(&bus, 4, 24, DgxScreenRGB, ST7789_RESOLUTION_240x320), false);
}

static void record_st7735(void)
{
    static const int depths[] = {12, 16, 18};
    for (size_t k = 0; k <= sizeof(depths) / sizeof(depths[0]); ++k) {
        const bool              bgr = k == sizeof(depths) / sizeof(depths[0]);
        const int               bits = bgr ? 16 : depths[k];
        const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
        dgx_bus_protocols_t     bus = bus_of(DGX_BUS_SPI);
        part("st7735", "", bits, order);
        dgx_screen_t *s = dgx_st7735_init(&bus, 4, (uint8_t)bits, order);
        if (s) turns(s, dgx_st7735_orientation, !bgr, false);
        done(s, true);
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_SPI);
    part("st7735", " refused", 24, DgxScreenRGB);
    done(dgx_st7735_init(&bus, 4, 24, DgxScreenRGB), false);
}

static void record_gc9a01(void)
{
    static const int depths[] = {12, 16, 18};
    for (size_t k = 0; k <= sizeof(depths) / sizeof(depths[0]); ++k) {
        const bool              bgr = k == sizeof(depths) / sizeof(depths[0]);
        const int               bits = bgr ? 16 : depths[k];
        const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
        dgx_bus_protocols_t     bus = bus_of(DGX_BUS_SPI);
        part("gc9a01", "", bits, order);
        dgx_screen_t *s = dgx_gc9a01_init(&bus, 4, (uint8_t)bits, order);
        if (s) {
            turns(s, dgx_gc9a01_orientation, !bgr, false);
            dgx_gc9a01_display_off(s);
            dgx_gc9a01_display_on(s);
        }
        done(s, true);
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_SPI);
    part("gc9a01", " refused", 24, DgxScreenRGB);
    done(dgx_gc9a01_init(&bus, 4, 24, DgxScreenRGB), false);
}

static void record_ssd1351(void)
{
    static const int depths[] = {16, 18};
    for (size_t k = 0; k <= sizeof(depths) / sizeof(depths[0]); ++k) {
        const bool              bgr = k == sizeof(depths) / sizeof(depths[0]);
        const int               bits = bgr ? 16 : depths[k];
        const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
        dgx_bus_protocols_t     bus = bus_of(DGX_BUS_SPI);
        part("ssd1351", "", bits, order);
        dgx_screen_t *s = dgx_ssd1351_init(&bus, 4, (uint8_t)bits, order);
        if (s) {
            turns(s, dgx_ssd1351_orientation, !bgr, false);
            dgx_ssd1351_brightness(s, 0x80);
        }
        done(s, true);
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_SPI);
    part("ssd1351", " refused", 12, DgxScreenRGB);
    done(dgx_ssd1351_init(&bus, 4, 12, DgxScreenRGB), false);
}

static void record_nv3041a(void)
{
    for (int bgr = 0; bgr < 2; ++bgr) {
        const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
        dgx_bus_protocols_t     bus = bus_of(DGX_BUS_QSPI);
        part("nv3041a", "", 16, order);
        dgx_screen_t *s = dgx_nv3041a_init(&bus, 4, 1, 16, order);
        if (s) {
            turns(s, dgx_nv3041a_orientation, !bgr, false);
            dgx_nv3041a_backlight(s, false);
            dgx_nv3041a_backlight(s, true);
        }
        done(s, true);
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_QSPI);
    part("nv3041a", " refused", 18, DgxScreenRGB);
    done(dgx_nv3041a_init(&bus, 4, 1, 18, DgxScreenRGB), false);
}

static void record_st77916(void)
{
    static const int depths[] = {16, 18};
    for (size_t k = 0; k <= sizeof(depths) / sizeof(depths[0]); ++k) {
        const bool              bgr = k == sizeof(depths) / sizeof(depths[0]);
        const int               bits = bgr ? 16 : depths[k];
        const dgx_color_order_t order = bgr ? DgxScreenBGR : DgxScreenRGB;
        dgx_bus_protocols_t     bus = bus_of(DGX_BUS_QSPI);
        part("st77916", "", bits, order);
        dgx_screen_t *s = dgx_st77916_init(&bus, 4, 1, (uint8_t)bits, order);
        if (s) {
            turns(s, dgx_st77916_orientation, !bgr, false);
            dgx_st77916_backlight(s, false);
            dgx_st77916_backlight(s, true);
        }
        done(s, true);
    }
    dgx_bus_protocols_t bus = bus_of(DGX_BUS_QSPI);
    part("st77916", " refused", 12, DgxScreenRGB);
    done(dgx_st77916_init(&bus, 4, 1, 12, DgxScreenRGB), false);
}

/* ---- the comparison ---- */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    *len       = fread(text, 1, (size_t)n, f);
    text[*len] = 0;
    fclose(f);
    return text;
}

/* the first line that differs, with the heading of the part and of the turn it is in */
static void show_difference(const char *expected, const char *got)
{
    const char *e = expected, *g = got, *part_at = got, *turn_at = NULL;
    long        line = 1;
    while (*e && *g) {
        const char *e_end = strchr(e, '\n'), *g_end = strchr(g, '\n');
        if (!e_end || !g_end) break;
        if (!strncmp(g, "## ", 3)) part_at = g, turn_at = NULL;
        if (!strncmp(g, "# ", 2)) turn_at = g;
        if (e_end - e != g_end - g || memcmp(e, g, (size_t)(e_end - e))) break;
        e = e_end + 1, g = g_end + 1, ++line;
    }
    printf("line %ld of %s differs, in %.*s", line, STREAMS, (int)(strchr(part_at, '\n') - part_at + 1), part_at);
    if (turn_at) printf("  at %.*s", (int)(strchr(turn_at, '\n') - turn_at + 1), turn_at);
    printf("  expected: %.*s\n  got:      %.*s\n", (int)strcspn(e, "\n"), e, (int)strcspn(g, "\n"), g);
}

int main(int argc, char **argv)
{
    pixels_end();
    record_ili9341();
    record_st7789();
    record_st7735();
    record_gc9a01();
    record_ssd1351();
    record_nv3041a();
    record_st77916();
    if (argc > 1 && !strcmp(argv[1], "write")) {
        FILE *f = fopen(STREAMS, "wb");
        CHECK(f && fwrite(record, 1, record_len, f) == record_len);
        if (f) fclose(f);
        printf("%s written: %zu bytes\n", STREAMS, record_len);
    } else {
        size_t expected_len = 0;
        char  *expected     = read_file(STREAMS, &expected_len);
        CHECK(expected != NULL);
        if (expected) {
            const bool same = expected_len == record_len && !memcmp(expected, record, record_len);
            if (!same) show_difference(expected, record);
            CHECK(same);
            free(expected);
        }
    }
    free(record);
    CHECK_DONE();
}
