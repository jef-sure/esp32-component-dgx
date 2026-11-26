/*
 * Pictures read of the bytes of a .dgxi file, dgx_image_file.h: small files of every kind made here, read at an
 * address divisible by 4 and at the three after it, give their pictures where they lie. Cut at every length and
 * spoiled byte by byte, a file is refused or gives pictures whose every byte, and every byte of their planes, lies in
 * the file: the tests run under AddressSanitizer and UndefinedBehaviorSanitizer, so a read past the file is a fault.
 * A file of a later format is told apart; an indexed picture is drawn by dgx_image_blit() as the file says.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_image_file.h"
#include "dgx_vscreen.h"

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static uint32_t up4(uint32_t n) { return (n + 3u) & ~3u; }

typedef struct {
    uint8_t     bits, kind;
    int         count;
    int         w[3], h[3];
    int         colors;
    bool        plane;
    const char *about;
} spec_t;

static uint32_t bytes_of(const spec_t *s, int w, int h)
{
    if (s->kind != DGX_IMAGE_FILE_PIXELS) return (uint32_t)((w * s->bits + 7) / 8 * h);
    return (uint32_t)(((uint64_t)w * h * (s->bits == 18 ? 24 : s->bits) + 7) / 8);
}

/* a file as tools/dgx_images.py writes one: the head, the table, the palette, the pixels and planes, the text */
static uint32_t make(const spec_t *s, uint8_t *out)
{
    memset(out, 0, 4096);
    uint32_t at = up4(64 + 16 * s->count), palette = 0;
    if (s->kind != DGX_IMAGE_FILE_PIXELS) palette = at, at = up4(at + 4 * s->colors);
    for (int i = 0; i < s->colors; ++i) out[palette + 4 * i] = (uint8_t)(10 * i), out[palette + 4 * i + 3] = 255;
    for (int k = 0; k < s->count; ++k) {
        uint8_t *e = out + 64 + 16 * k;
        put16(e, s->w[k]), put16(e + 2, s->h[k]), put16(e + 4, (uint32_t)-k), put16(e + 6, k);
        const uint32_t n = bytes_of(s, s->w[k], s->h[k]);
        put32(e + 8, at);
        for (uint32_t i = 0; i < n; ++i) out[at + i] = (uint8_t)(i * 7 + k);
        if (s->kind == DGX_IMAGE_FILE_INDEXED && s->bits == 4)
            for (uint32_t i = 0; i < n; ++i) out[at + i] &= 0x11; /* values 0 and 1: an entry for each */
        at = up4(at + n);
        if (s->plane) {
            put32(e + 12, at);
            for (int i = 0; i < s->w[k] * s->h[k]; ++i) out[at + i] = (uint8_t)(i * 13);
            at = up4(at + (uint32_t)(s->w[k] * s->h[k]));
        }
    }
    const uint32_t about = s->about ? at : 0, about_length = s->about ? (uint32_t)strlen(s->about) : 0;
    if (s->about) memcpy(out + at, s->about, about_length), at += about_length;
    memcpy(out, "DGXI", 4);
    put16(out + 4, s->kind == DGX_IMAGE_FILE_MASK ? 2 : 1);
    out[6] = s->bits, out[7] = s->kind;
    put16(out + 8, s->count), put16(out + 10, s->colors);
    put32(out + 12, at), put32(out + 16, 64), put32(out + 20, palette), put32(out + 24, about), put32(out + 28, about_length);
    return at;
}

static volatile unsigned sum; /* the bytes read, kept so that the reads are made */

/* every byte of every picture and plane read, and the indexed ones drawn: a read outside the file is a fault */
static void use(const dgx_image_file_t *f)
{
    for (unsigned k = 0; k < f->count; ++k) {
        dgx_image_file_picture_t p;
        CHECK(dgx_image_file_picture(f, k, &p) == ESP_OK);
        const spec_t   s = {.bits = f->bits, .kind = f->kind};
        const uint32_t n = bytes_of(&s, p.width, p.height);
        for (uint32_t i = 0; i < n; ++i) sum += p.pixels[i];
        if (p.alpha)
            for (uint32_t i = 0; i < (uint32_t)p.width * p.height; ++i) sum += p.alpha[i];
        for (unsigned i = 0; i < 4u * f->colors; ++i) sum += f->palette[i];
        for (uint32_t i = 0; i < f->about_length; ++i) sum += (uint8_t)f->about[i];
        dgx_image_t image;
        if (dgx_image_file_image(f, k, &image) == ESP_OK && (uint32_t)p.width * p.height <= 4096) {
            /* indexed by values into 8 bits; every picture by its colors, soft, into 16 bits */
            dgx_screen_t *s8 = dgx_vscreen_init(p.width, p.height, 8, DgxScreenRGB), *s16 = dgx_vscreen_init(p.width, p.height, 16, DgxScreenRGB);
            if (s8 && f->kind == DGX_IMAGE_FILE_INDEXED) dgx_image_blit(s8, p.origin_x, p.origin_y, &image, NULL);
            if (s16) {
                static uint32_t colors[256];
                static uint8_t  alpha[256];
                dgx_image_blit_t how = {.flags = DGX_IMAGE_SOFT, .color = 0xffff};
                if (f->kind == DGX_IMAGE_FILE_INDEXED && dgx_image_file_colors(f, s16, colors, alpha) == ESP_OK) how.colors = colors, how.alpha = alpha;
                if (f->kind == DGX_IMAGE_FILE_MASK) how.flags |= DGX_IMAGE_SILHOUETTE;
                dgx_image_blit(s16, p.origin_x, p.origin_y, &image, &how);
                how.flags |= DGX_IMAGE_OPAQUE;
                how.orientation = DgxOutputRotate90CW;
                dgx_image_blit(s16, p.origin_x, p.origin_y, &image, &how);
            }
            dgx_screen_destroy(&s8);
            dgx_screen_destroy(&s16);
        }
    }
}

/* the file of size bytes at an address 4k + shift, in a buffer of exactly its bytes */
static esp_err_t open_at(const uint8_t *file, uint32_t size, unsigned shift, dgx_image_file_t *f, uint8_t **buffer)
{
    *buffer = malloc(size + shift + 1);
    uint8_t *b = *buffer + shift;
    memcpy(b, file, size);
    return dgx_image_file_open(b, size, f);
}

int main(void)
{
    static const spec_t specs[] = {
        {.bits = 4, .kind = DGX_IMAGE_FILE_INDEXED, .count = 2, .w = {3, 5}, .h = {2, 1}, .colors = 2, .about = "name: test\nsource: a.png"},
        {.bits = 1, .kind = DGX_IMAGE_FILE_INDEXED, .count = 1, .w = {9}, .h = {3}, .colors = 2},
        {.bits = 8, .kind = DGX_IMAGE_FILE_INDEXED, .count = 1, .w = {4}, .h = {4}, .colors = 256},
        {.bits = 16, .kind = DGX_IMAGE_FILE_PIXELS, .count = 2, .w = {3, 2}, .h = {3, 2}, .plane = true},
        {.bits = 12, .kind = DGX_IMAGE_FILE_PIXELS, .count = 1, .w = {5}, .h = {3}},
        {.bits = 1, .kind = DGX_IMAGE_FILE_PIXELS, .count = 3, .w = {9, 1, 7}, .h = {3, 1, 2}, .plane = true},
        {.bits = 32, .kind = DGX_IMAGE_FILE_PIXELS, .count = 1, .w = {2}, .h = {2}, .about = "x"},
        {.bits = 18, .kind = DGX_IMAGE_FILE_PIXELS, .count = 1, .w = {3}, .h = {1}},
        {.bits = 4, .kind = DGX_IMAGE_FILE_MASK, .count = 2, .w = {5, 3}, .h = {2, 3}, .colors = 1},
        {.bits = 8, .kind = DGX_IMAGE_FILE_MASK, .count = 1, .w = {3}, .h = {2}, .colors = 1, .about = "mask"},
    };
    static uint8_t file[4096], spoiled[4096];
    for (size_t s = 0; s < sizeof specs / sizeof specs[0]; ++s) {
        const spec_t  *sp   = &specs[s];
        const uint32_t size = make(sp, file);
        for (unsigned shift = 0; shift < 4; ++shift) {
            dgx_image_file_t f;
            uint8_t         *buffer;
            if (open_at(file, size, shift, &f, &buffer) != ESP_OK) {
                CHECK(!"a sound file is read");
                free(buffer);
                continue;
            }
            const uint8_t *b = buffer + shift;
            CHECK(f.bits == sp->bits && f.kind == sp->kind && f.count == sp->count && f.colors == sp->colors && f.length == size);
            CHECK(f.first == (sp->kind == DGX_IMAGE_FILE_INDEXED && sp->colors < 1 << sp->bits ? 1 : 0));
            CHECK((f.palette != NULL) == (sp->kind != DGX_IMAGE_FILE_PIXELS));
            CHECK(sp->about ? f.about_length == strlen(sp->about) && !memcmp(f.about, sp->about, f.about_length) : f.about == NULL);
            for (int k = 0; k < sp->count; ++k) {
                dgx_image_file_picture_t p;
                CHECK(dgx_image_file_picture(&f, (unsigned)k, &p) == ESP_OK);
                CHECK(p.width == sp->w[k] && p.height == sp->h[k] && p.origin_x == -k && p.origin_y == k);
                CHECK(p.pixels > b && (p.pixels - b) % 4 == 0 && (p.alpha != NULL) == sp->plane);
                dgx_image_t image;
                if (sp->kind != DGX_IMAGE_FILE_PIXELS || sp->bits == 16) {
                    CHECK(dgx_image_file_image(&f, (unsigned)k, &image) == ESP_OK);
                    CHECK(image.width == p.width && image.height == p.height && image.origin_x == p.origin_x && image.format == sp->bits &&
                          image.data == p.pixels && image.alpha == p.alpha);
                } else {
                    CHECK(dgx_image_file_image(&f, (unsigned)k, &image) == ESP_ERR_INVALID_ARG);
                }
            }
            dgx_image_file_picture_t past;
            CHECK(dgx_image_file_picture(&f, (unsigned)sp->count, &past) == ESP_ERR_INVALID_ARG);
            use(&f);
            free(buffer);
        }
        /* cut at every length: refused, and with the length of the head cut too, refused or whole */
        for (uint32_t cut = 0; cut < size; ++cut) {
            dgx_image_file_t f;
            uint8_t         *buffer;
            CHECK(open_at(file, cut, cut % 4, &f, &buffer) != ESP_OK);
            free(buffer);
            memcpy(spoiled, file, size);
            if (cut >= 16) put32(spoiled + 12, cut);
            if (open_at(spoiled, cut, cut % 4, &f, &buffer) == ESP_OK) use(&f);
            free(buffer);
        }
        /* every byte spoiled four ways */
        for (uint32_t at = 0; at < size; ++at) {
            for (int way = 0; way < 4; ++way) {
                memcpy(spoiled, file, size);
                spoiled[at] = way == 0 ? 0x00 : way == 1 ? 0xFF : way == 2 ? spoiled[at] ^ 0x80 : (uint8_t)(spoiled[at] + 1);
                dgx_image_file_t f;
                uint8_t         *buffer;
                const esp_err_t  e = open_at(spoiled, size, at % 4, &f, &buffer);
                CHECK(e == ESP_OK || e == ESP_ERR_INVALID_ARG || e == ESP_ERR_INVALID_VERSION);
                if (e == ESP_OK) use(&f);
                free(buffer);
            }
        }
    }
    /* a later format, a mask in a file of format 1, NULLs */
    const uint32_t size = make(&specs[0], file);
    put16(file + 4, DGX_IMAGE_FILE_VERSION + 1);
    dgx_image_file_t f;
    CHECK(dgx_image_file_open(file, size, &f) == ESP_ERR_INVALID_VERSION);
    const uint32_t masked = make(&specs[sizeof specs / sizeof specs[0] - 1], file);
    CHECK(dgx_image_file_open(file, masked, &f) == ESP_OK && f.kind == DGX_IMAGE_FILE_MASK && f.colors == 1 && f.palette);
    put16(file + 4, 1);
    CHECK(dgx_image_file_open(file, masked, &f) == ESP_ERR_INVALID_ARG);
    /* the colors of the values of an indexed file on a screen: the palette, 0 transparent when it is not full */
    {
        static const spec_t two = {.bits = 4, .kind = DGX_IMAGE_FILE_INDEXED, .count = 1, .w = {2}, .h = {1}, .colors = 2};
        const uint32_t      n   = make(&two, file);
        CHECK(dgx_image_file_open(file, n, &f) == ESP_OK);
        dgx_screen_t *s16 = dgx_vscreen_init(4, 4, 16, DgxScreenRGB);
        uint32_t      colors[256];
        uint8_t       alpha[256];
        CHECK(dgx_image_file_colors(&f, s16, colors, alpha) == ESP_OK);
        CHECK(alpha[0] == 0 && colors[1] == dgx_rgb_to_16(0, 0, 0) && alpha[1] == 255 && colors[2] == dgx_rgb_to_16(10, 0, 0) && alpha[2] == 255 &&
              alpha[3] == 0);
        CHECK(dgx_image_file_colors(NULL, s16, colors, alpha) == ESP_ERR_INVALID_ARG);
        dgx_screen_destroy(&s16);
    }
    CHECK(dgx_image_file_open(NULL, size, &f) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_image_file_open(file, size, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_image_file_picture(NULL, 0, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_image_file_image(NULL, 0, NULL) == ESP_ERR_INVALID_ARG);

    /* an indexed picture drawn as the file says: 4 bits, values 0 and 1, 0 transparent, 1 through no map */
    {
        static const spec_t one = {.bits = 4, .kind = DGX_IMAGE_FILE_INDEXED, .count = 1, .w = {4}, .h = {2}, .colors = 1};
        const uint32_t      n   = make(&one, file);
        const uint8_t       px[] = {0x10, 0x01, 0x11, 0x00}; /* rows 1 0 0 1 / 1 1 0 0 */
        memcpy(file + up4(64 + 16) + 4, px, sizeof px);
        CHECK(dgx_image_file_open(file, n, &f) == ESP_OK);
        dgx_image_t image;
        CHECK(dgx_image_file_image(&f, 0, &image) == ESP_OK);
        dgx_screen_t *scr = dgx_vscreen_init(4, 2, 8, DgxScreenRGB);
        dgx_fill_rectangle(scr, 0, 0, 4, 2, 9);
        CHECK(dgx_image_blit(scr, 0, 0, &image, NULL) == ESP_OK); /* the origin of the picture is 0 0 */
        static const uint8_t want[8] = {1, 9, 9, 1, 1, 1, 9, 9};
        for (int i = 0; i < 8; ++i) CHECK(dgx_get_pixel(scr, i % 4, i / 4) == want[i]);
        dgx_screen_destroy(&scr);
    }
    CHECK_DONE();
}
