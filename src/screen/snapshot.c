/*
 * snapshot.c: a snapshot of a screen as a .dgxi file, see dgx_snapshot.h; the format is told in
 * src/vscreen/image_file.c.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */
#include "dgx_snapshot.h"

#include <string.h>

#include "dgx_draw.h"
#include "dgx_vscreen.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

static const char *TAG = "DGX SNAPSHOT";

/* reading the memory of a virtual screen row by row needs vscreen.c, which is optional in the component build */
#if !defined(ESP_PLATFORM) || defined(CONFIG_DGX_ENABLE_VSCREEN)
#define SNAPSHOT_LINEAR 1
#else
#define SNAPSHOT_LINEAR 0
#endif

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }

/* the bytes of a snapshot gathered into pieces for the writer */
typedef struct {
    dgx_snapshot_write_t write;
    void                *ctx;
    uint8_t              piece[256];
    size_t               n;
    bool                 failed;
    uint32_t             acc; /* bits not yet a whole byte, the first in the high ones */
    unsigned             bits;
} sink_t;

static void sink_flush(sink_t *s)
{
    if (s->n && !s->failed && !s->write(s->ctx, s->piece, s->n)) s->failed = true;
    s->n = 0;
}

static void sink_bytes(sink_t *s, const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    while (n && !s->failed) {
        const size_t room = sizeof s->piece - s->n, k = n < room ? n : room;
        memcpy(s->piece + s->n, b, k);
        s->n += k, b += k, n -= k;
        if (s->n == sizeof s->piece) sink_flush(s);
    }
}

/* a value of `width` bits, the high bit first, after the bits before it: the layout of a virtual screen */
static void sink_value(sink_t *s, uint32_t value, unsigned width)
{
    for (unsigned k = width; k;) {
        const unsigned take = k > 8 ? 8 : k;
        k -= take;
        s->acc = s->acc << take | ((value >> k) & ((1u << take) - 1u));
        s->bits += take;
        if (s->bits >= 8) {
            s->bits -= 8;
            const uint8_t byte = (uint8_t)(s->acc >> s->bits);
            sink_bytes(s, &byte, 1);
        }
    }
}

esp_err_t dgx_screen_snapshot(dgx_screen_t *scr, int x, int y, int w, int h, const char *name, dgx_snapshot_write_t write, void *ctx)
{
    if (!scr || !write) return ESP_ERR_INVALID_ARG;
    const uint8_t bits = scr->color_bits;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 12 && bits != 16 && bits != 18 && bits != 24 && bits != 32) {
        ESP_LOGE(TAG, "a screen of %u bits: a file of pictures holds 1, 4, 8, 12, 16, 18, 24 or 32", bits);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!dgx_screen_is_readable(scr)) {
        ESP_LOGE(TAG, "the screen cannot be read: take the virtual screen the program draws in");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (w <= 0 || h <= 0) x = 0, y = 0, w = scr->width, h = scr->height;
    if (x < 0 || y < 0 || x >= scr->width || y >= scr->height) return ESP_ERR_INVALID_ARG;
    if (w > scr->width - x) w = scr->width - x;
    if (h > scr->height - y) h = scr->height - y;
    if (w > 65535 || h > 65535) return ESP_ERR_INVALID_ARG;

    /* the head, the one picture of the table, the pixels from byte 80, the text after them */
    const unsigned stored = bits == 18 ? 24u : bits;
    const uint64_t pixels = ((uint64_t)w * (uint64_t)h * stored + 7u) / 8u;
    char           about[96];
    const int      about_n = snprintf(about, sizeof about, "name: %s\nmade-by: dgx_screen_snapshot of DGX", name ? name : "snapshot");
    const uint64_t length  = 80u + pixels + (uint64_t)(about_n > 0 ? about_n : 0);
    if (length > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t head[80] = {0};
    memcpy(head, "DGXI", 4);
    put16(head + 4, 1), head[6] = bits, head[7] = 2; /* format 1, pictures of pixels */
    put16(head + 8, 1), put32(head + 12, (uint32_t)length), put32(head + 16, 64);
    put32(head + 24, (uint32_t)(80u + pixels)), put32(head + 28, (uint32_t)(about_n > 0 ? about_n : 0));
    put16(head + 64, (uint32_t)w), put16(head + 66, (uint32_t)h), put32(head + 72, 80);

    sink_t s = {.write = write, .ctx = ctx};
    sink_bytes(&s, head, sizeof head);
#if SNAPSHOT_LINEAR
    if (stored % 8u == 0 && dgx_vscreen_is_linear(scr)) {
        /* memory laid out as the file lays it: the rows as they are */
        const size_t   bytes = stored / 8u;
        const uint8_t *array = ((dgx_vscreen_t *)scr)->v_array;
        for (int r = 0; r < h && !s.failed; ++r) sink_bytes(&s, array + ((size_t)(y + r) * (size_t)scr->width + (size_t)x) * bytes, (size_t)w * bytes);
    } else
#endif
    {
        for (int r = 0; r < h && !s.failed; ++r)
            for (int c = 0; c < w; ++c) {
                uint32_t v = dgx_get_pixel(scr, x + c, y + r);
                if (bits == 4 || bits == 12) v >>= 4; /* a level or the nibbles of a color, kept from bit 4 up */
                sink_value(&s, v, stored);
            }
        if (s.bits) sink_value(&s, 0, 8u - s.bits); /* the last byte, its low bits 0 */
    }
    if (about_n > 0) sink_bytes(&s, about, (size_t)about_n);
    sink_flush(&s);
    return s.failed ? ESP_FAIL : ESP_OK;
}

/* ---- the console ---- */

typedef struct {
    FILE    *out;
    uint8_t  three[3];
    unsigned have, column;
    uint32_t crc, length, since_rest;
} print_t;

/*
 * A console at 115200 baud takes some 11 KB a second, and its writes wait for the line without giving the processor
 * away: a snapshot of 100 KB keeps the task busy for ten seconds, longer than the watchdog lets the idle task go
 * without a turn, whose message then cuts a line of the snapshot. A tick is given away every 4 KB of output.
 */
#define PRINT_REST_BYTES 4096u

static uint32_t crc32_update(uint32_t crc, const uint8_t *b, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; ++k) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void print_group(print_t *p, unsigned n)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint32_t    v     = (uint32_t)p->three[0] << 16 | (uint32_t)(n > 1 ? p->three[1] : 0) << 8 | (n > 2 ? p->three[2] : 0);
    char              four[4];
    for (int k = 0; k < 4; ++k) four[k] = k <= (int)n ? abc[v >> (18 - 6 * k) & 63u] : '=';
    fwrite(four, 1, 4, p->out);
    p->column += 4;
    if (p->column == 76) {
        fputc('\n', p->out), p->column = 0;
        if ((p->since_rest += 77u) >= PRINT_REST_BYTES) {
            fflush(p->out);
            vTaskDelay(1);
            p->since_rest = 0;
        }
    }
}

static bool print_bytes(void *ctx, const void *bytes, size_t n)
{
    print_t       *p = ctx;
    const uint8_t *b = bytes;
    p->crc = crc32_update(p->crc, b, n);
    p->length += (uint32_t)n;
    for (size_t i = 0; i < n; ++i) {
        p->three[p->have++] = b[i];
        if (p->have == 3) print_group(p, 3), p->have = 0;
    }
    return !ferror(p->out);
}

esp_err_t dgx_screen_snapshot_print(dgx_screen_t *scr, int x, int y, int w, int h, const char *name, FILE *out)
{
    if (!name || !*name) return ESP_ERR_INVALID_ARG;
    for (const char *c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.'))
            return ESP_ERR_INVALID_ARG;
    print_t p = {.out = out ? out : stdout};
    fprintf(p.out, "\nDGXSNAP BEGIN %s\n", name);
    const esp_err_t e = dgx_screen_snapshot(scr, x, y, w, h, name, print_bytes, &p);
    if (p.have) print_group(&p, p.have);
    if (p.column) fputc('\n', p.out);
    if (e == ESP_OK) fprintf(p.out, "DGXSNAP END %s %u %08x\n", name, (unsigned)p.length, (unsigned)p.crc);
    fflush(p.out);
    return e;
}
