/*
 * image_file.c: the pictures of the bytes of a .dgxi file, see dgx_image_file.h; tools/dgx_images.py writes them.
 *
 *  Copyright (c) 2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 * The head, 64 bytes, numbers little-endian:
 *    0  4  "DGXI"
 *    4  2  the version of the format, 1
 *    6  1  bits a pixel: 1, 4, 8, 12, 16, 18, 24, 32
 *    7  1  the kind: 1 indexed, 2 pixels
 *    8  2  pictures, at least 1
 *   10  2  entries of the palette: 1 to 2^bits for indexed pictures, 0 for pixels
 *   12  4  the length of the file
 *   16  4  where the table of pictures starts
 *   20  4  where the palette starts, 0 for none
 *   24  4  where the text about the pictures starts, 0 for none
 *   28  4  the length of the text
 *   32 32  0, reserved
 * A picture of the table, 16 bytes: width 2, height 2, origin_x 2 and origin_y 2 with a sign, where its pixels start
 * 4, where its plane of opacity starts 4, 0 for none. The table, the palette, the pixels and the planes start at
 * offsets divisible by 4.
 */
#include "dgx_image_file.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "DGX IMAGE FILE";

enum {
    HEAD    = 64,
    PICTURE = 16,
    ENTRY   = 4,
};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t  i16(const uint8_t *p) { return (int16_t)u16(p); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static esp_err_t spoiled(const char *what, uint32_t at)
{
    ESP_LOGE(TAG, "not a file of pictures DGX reads: %s, at byte %u", what, (unsigned)at);
    return ESP_ERR_INVALID_ARG;
}

/* size bytes from offset, after the head, inside the file and from a multiple of 4 */
static bool inside(uint32_t length, uint32_t offset, uint64_t size)
{
    return offset >= HEAD && offset % 4 == 0 && (uint64_t)offset + size <= length;
}

/* the bytes of the pixels of a picture: rows from a new byte for indexed ones, rows one after another for pixels */
static uint64_t pixel_bytes(uint8_t kind, uint8_t bits, uint32_t w, uint32_t h)
{
    if (kind == DGX_IMAGE_FILE_INDEXED) return ((uint64_t)w * bits + 7) / 8 * h;
    const uint64_t stored = bits == 18 ? 24 : bits; /* an 18-bit pixel is kept in 3 bytes */
    return ((uint64_t)w * h * stored + 7) / 8;
}

static bool a_depth(uint8_t bits)
{
    return bits == 1 || bits == 4 || bits == 8 || bits == 12 || bits == 16 || bits == 18 || bits == 24 || bits == 32;
}

esp_err_t dgx_image_file_open(const void *bytes, size_t length, dgx_image_file_t *file)
{
    const uint8_t *b = bytes;
    if (!file) return spoiled("no place for the file given", 0);
    if (!b || length < HEAD) return spoiled("shorter than its head", 0);
    if (memcmp(b, "DGXI", 4)) return spoiled("no mark DGXI", 0);
    if (u16(b + 4) != DGX_IMAGE_FILE_VERSION) {
        ESP_LOGE(TAG, "a file of pictures of format %u, of a later DGX: this one reads format %d", u16(b + 4), DGX_IMAGE_FILE_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    const uint8_t  bits = b[6], kind = b[7];
    const uint16_t count = u16(b + 8), colors = u16(b + 10);
    const uint32_t size = u32(b + 12), table = u32(b + 16), palette = u32(b + 20), about = u32(b + 24), about_length = u32(b + 28);
    if (size < HEAD || size > length) return spoiled("its length is more than the bytes given, or less than a head", 12);
    if (!a_depth(bits)) return spoiled("no depth of a virtual screen", 6);
    if (kind != DGX_IMAGE_FILE_INDEXED && kind != DGX_IMAGE_FILE_PIXELS) return spoiled("no kind of pictures", 7);
    if (kind == DGX_IMAGE_FILE_INDEXED && bits != 1 && bits != 4 && bits != 8) return spoiled("indexed pictures of other bits than 1, 4, 8", 6);
    if (!count) return spoiled("no picture", 8);
    if (kind == DGX_IMAGE_FILE_INDEXED) {
        if (!colors || colors > 1u << bits) return spoiled("the number of colors of the palette", 10);
        if (!inside(size, palette, (uint64_t)colors * ENTRY)) return spoiled("the palette", 20);
    } else if (colors || palette) {
        return spoiled("a palette of pictures of pixels", 10);
    }
    if (!inside(size, table, (uint64_t)count * PICTURE)) return spoiled("the table of pictures", 16);
    if (about && (about < HEAD || (uint64_t)about + about_length > size)) return spoiled("the text about the pictures", 24);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = table + i * PICTURE;
        const uint8_t *p  = b + at;
        const uint16_t w = u16(p), h = u16(p + 2);
        const uint32_t pixels = u32(p + 8), alpha = u32(p + 12);
        if (!w || !h) return spoiled("a picture of no pixels", at);
        const uint64_t n = pixel_bytes(kind, bits, w, h);
        if (n > UINT32_MAX || !inside(size, pixels, n)) return spoiled("the pixels of a picture", at + 8);
        if (alpha) {
            /* the opacity of an indexed picture is in its palette, that of 32 bits in its pixels */
            if (kind == DGX_IMAGE_FILE_INDEXED || bits == 32) return spoiled("a plane of opacity of a picture that has its opacity elsewhere", at + 12);
            if (!inside(size, alpha, (uint64_t)w * h)) return spoiled("the plane of opacity of a picture", at + 12);
        }
    }
    *file = (dgx_image_file_t){
        .bits         = bits,
        .kind         = kind,
        .count        = count,
        .colors       = colors,
        .first        = kind == DGX_IMAGE_FILE_INDEXED && colors < 1u << bits ? 1 : 0, /* a full palette has no transparent value */
        .palette      = kind == DGX_IMAGE_FILE_INDEXED ? b + palette : NULL,
        .about        = about ? (const char *)b + about : NULL,
        .about_length = about ? about_length : 0,
        .bytes        = b,
        .length       = size,
    };
    return ESP_OK;
}

esp_err_t dgx_image_file_picture(const dgx_image_file_t *file, unsigned index, dgx_image_file_picture_t *picture)
{
    if (!file || !file->bytes || !picture) {
        ESP_LOGE(TAG, "no file or no place for the picture given");
        return ESP_ERR_INVALID_ARG;
    }
    if (index >= file->count) {
        ESP_LOGE(TAG, "picture %u of a file of %u", index, (unsigned)file->count);
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *p     = file->bytes + u32(file->bytes + 16) + index * PICTURE;
    const uint32_t alpha = u32(p + 12);
    *picture             = (dgx_image_file_picture_t){
                    .width    = u16(p),
                    .height   = u16(p + 2),
                    .origin_x = i16(p + 4),
                    .origin_y = i16(p + 6),
                    .pixels   = file->bytes + u32(p + 8),
                    .alpha    = alpha ? file->bytes + alpha : NULL,
    };
    return ESP_OK;
}

esp_err_t dgx_image_file_image(const dgx_image_file_t *file, unsigned index, dgx_image_t *image)
{
    if (!image) {
        ESP_LOGE(TAG, "no place for the picture given");
        return ESP_ERR_INVALID_ARG;
    }
    dgx_image_file_picture_t p;
    const esp_err_t          e = dgx_image_file_picture(file, index, &p);
    if (e != ESP_OK) return e;
    if (file->kind != DGX_IMAGE_FILE_INDEXED) {
        ESP_LOGE(TAG, "a picture of pixels is drawn as a virtual screen over its bytes, not as a dgx_image_t");
        return ESP_ERR_INVALID_ARG;
    }
    *image = (dgx_image_t){
        .width    = p.width,
        .height   = p.height,
        .origin_x = p.origin_x,
        .origin_y = p.origin_y,
        .format   = file->bits,
        .data     = p.pixels,
    };
    return ESP_OK;
}
