/*
 * A snapshot of a screen, dgx_snapshot.h: virtual screens of every depth a file of pictures holds, with pixels of
 * every value drawn into them, taken whole and by a rectangle whose edges cut bytes, read back by dgx_image_file.h:
 * every pixel the one of the screen. The console form, base64 between its marks, ends with the length and the
 * CRC-32 of the file it carries. A screen that cannot be read is refused, and a writer that stops stops it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#include "driver/gpio.h" /* the waits of the init table, for nothing on the host */
#include "drivers/dgx_lcd_init.c"
#include "drivers/panel.c"

#include "dgx_draw.h"
#include "dgx_image_file.h"
#include "dgx_snapshot.h"
#include "dgx_vscreen.h"

typedef struct {
    uint8_t *bytes;
    size_t   n, size;
    int      stop_after; /* calls before the writer says no; 0 for never, -1 for at once */
    int      calls;
} buffer_t;

static bool collect(void *ctx, const void *bytes, size_t n)
{
    buffer_t *b = ctx;
    if (b->stop_after && ++b->calls > b->stop_after) return false;
    if (b->n + n > b->size) b->bytes = realloc(b->bytes, b->size = (b->n + n) * 2);
    memcpy(b->bytes + b->n, bytes, n);
    b->n += n;
    return true;
}

/* a value of the depth for the pixel x, y: every value comes */
static uint32_t pattern(uint8_t bits, int x, int y)
{
    const uint32_t v = (uint32_t)(x * 131 + y * 71 + x * y * 7);
    switch (bits) {
    case 1: return v & 1u;
    case 4: return (v & 15u) << 4;
    case 8: return v & 0xffu;
    case 12: return (v * 2654435761u) & 0xfff0u;
    case 16: return (v * 2654435761u) & 0xffffu;
    case 18: return (v * 2654435761u) & 0xfcfcfcu;
    case 24: return (v * 2654435761u) & 0xffffffu;
    default: return v * 2654435761u;
    }
}

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t value) { (void)bus, (void)value; }
static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t bits) { (void)bus, (void)data, (void)bits; }
static void bus_sync(struct _dgx_bus_protocols_t *bus) { (void)bus; }

static const dgx_lcd_init_cmd_t no_init[] = {
    {0, 0, 0xff, 0, {0}}
};
static const dgx_panel_t panel = {.name = "panel", .width = 32, .height = 32, .colmod = {0, 0x55}, .init = no_init};

int main(void)
{
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    for (size_t d = 0; d < sizeof depths; ++d) {
        const uint8_t bits = depths[d];
        dgx_screen_t *scr  = dgx_vscreen_init(23, 9, bits, DgxScreenRGB);
        CHECK(scr != NULL);
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 23; ++x) dgx_set_pixel(scr, x, y, pattern(bits, x, y));
        static const int rects[][4] = {
            {0, 0, 0, 0},  /* the whole screen */
            {3, 2, 7, 5},  /* odd edges, rows that end inside a byte */
            {20, 6, 9, 9}, /* cut by the screen */
        };
        for (size_t r = 0; r < sizeof rects / sizeof rects[0]; ++r) {
            buffer_t b = {0};
            CHECK(dgx_screen_snapshot(scr, rects[r][0], rects[r][1], rects[r][2], rects[r][3], "test", collect, &b) == ESP_OK);
            dgx_image_file_t         f;
            dgx_image_file_picture_t p;
            CHECK(dgx_image_file_open(b.bytes, b.n, &f) == ESP_OK && f.bits == bits && f.kind == DGX_IMAGE_FILE_PIXELS && f.count == 1);
            CHECK(dgx_image_file_picture(&f, 0, &p) == ESP_OK);
            const int x0 = rects[r][2] > 0 ? rects[r][0] : 0, y0 = rects[r][3] > 0 ? rects[r][1] : 0;
            const int w = rects[r][2] > 0 ? (rects[r][2] < 23 - x0 ? rects[r][2] : 23 - x0) : 23;
            const int h = rects[r][3] > 0 ? (rects[r][3] < 9 - y0 ? rects[r][3] : 9 - y0) : 9;
            CHECK(p.width == w && p.height == h && f.about && memmem(f.about, f.about_length, "name: test", 10));
            dgx_screen_t *back = dgx_vscreen_from_array((void *)p.pixels, p.width, p.height, bits, DgxScreenRGB);
            int           same = back != NULL;
            for (int y = 0; same && y < h; ++y)
                for (int x = 0; x < w; ++x) same &= dgx_get_pixel(back, x, y) == dgx_get_pixel(scr, x0 + x, y0 + y);
            CHECK(same);
            dgx_screen_destroy(&back);
            free(b.bytes);
        }
        /* a writer that stops stops the snapshot */
        buffer_t stop = {.stop_after = -1};
        CHECK(dgx_screen_snapshot(scr, 0, 0, 0, 0, NULL, collect, &stop) == ESP_FAIL);
        free(stop.bytes);
        CHECK(dgx_screen_snapshot(scr, 23, 0, 1, 1, NULL, collect, &stop) == ESP_ERR_INVALID_ARG);
        dgx_screen_destroy(&scr);
    }

    /* the console form: base64 between the marks, the length and the CRC-32 of the file at the end */
    {
        dgx_screen_t *scr = dgx_vscreen_init(5, 3, 16, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, 5, 3, 0x1234);
        FILE *out = tmpfile();
        CHECK(dgx_screen_snapshot_print(scr, 0, 0, 0, 0, "shot-1.a", out) == ESP_OK);
        rewind(out);
        char line[128], end[64] = "";
        int  begin = 0, lines = 0;
        while (fgets(line, sizeof line, out)) {
            if (!strcmp(line, "DGXSNAP BEGIN shot-1.a\n")) begin = 1;
            else if (!strncmp(line, "DGXSNAP END ", 12)) snprintf(end, sizeof end, "%s", line);
            else if (begin && line[0] != '\n') ++lines;
        }
        buffer_t b = {0};
        CHECK(dgx_screen_snapshot(scr, 0, 0, 0, 0, "shot-1.a", collect, &b) == ESP_OK);
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < b.n; ++i) {
            crc ^= b.bytes[i];
            for (int k = 0; k < 8; ++k) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        char want[64];
        snprintf(want, sizeof want, "DGXSNAP END shot-1.a %u %08x\n", (unsigned)b.n, (unsigned)~crc);
        CHECK(begin && lines == (int)((b.n + 56) / 57) && !strcmp(end, want));
        CHECK(dgx_screen_snapshot_print(scr, 0, 0, 0, 0, "a b", out) == ESP_ERR_INVALID_ARG);
        CHECK(dgx_screen_snapshot_print(scr, 0, 0, 0, 0, "", out) == ESP_ERR_INVALID_ARG);
        fclose(out);
        free(b.bytes);
        dgx_screen_destroy(&scr);
    }

    /* a display behind a bus cannot be read */
    {
        static uint8_t      buf[256];
        dgx_bus_protocols_t bus = {.bus_type = DGX_BUS_SPI, .write_command = bus_command, .write_data = bus_data, .write_data_async = bus_data,
                                   .sync_write = bus_sync, .buffer = buf, .buffer_len = sizeof(buf)};
        dgx_screen_t       *lcd = dgx_panel_init(&bus, &panel, GPIO_NUM_NC, GPIO_NUM_NC, 16, DgxScreenRGB);
        buffer_t            b   = {0};
        CHECK(lcd && dgx_screen_snapshot(lcd, 0, 0, 0, 0, NULL, collect, &b) == ESP_ERR_NOT_SUPPORTED && b.n == 0);
        dgx_screen_destroy(&lcd);
    }
    CHECK(dgx_screen_snapshot(NULL, 0, 0, 0, 0, NULL, collect, NULL) == ESP_ERR_INVALID_ARG);
    CHECK_DONE();
}
