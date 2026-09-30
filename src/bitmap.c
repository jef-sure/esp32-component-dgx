#include <stdbool.h>
#include <string.h>

#include "dgx_bitmap.h"
#include "dgx_bits.h"

int dgx_bw_bitmap_foreach_set(dgx_bw_bitmap_t *bmap, dgx_bw_bitmap_pixel_func_t func, void *user_data)
{
    int visited = 0;
    if (bmap == NULL || bmap->bitmap == NULL) {
        return visited;
    }
    if (!bmap->is_stream) { // DGX_FONT_BITMAP_LINES: row-major, MSB-first
        int pitch = (bmap->width + 7) / 8;
        for (int y = 0; y < bmap->height; ++y) {
            const uint8_t *row = bmap->bitmap + (size_t)y * (size_t)pitch;
            for (int xb = 0; xb < pitch; ++xb) {
                uint8_t bits = row[xb];
                if (bits == 0) continue; // whole byte empty: skip 8 pixels
                int valid = bmap->width - xb * 8;
                if (valid < 8) bits &= (uint8_t)(0xffu << (8 - valid)); // drop row padding
                if (bits == 0) continue;
                if (func == NULL) {
                    visited += __builtin_popcount(bits);
                    continue;
                }
                int x_base = xb * 8;
                while (bits) {
                    int lead = __builtin_clz((unsigned)bits << 24); // 0..7, MSB first
                    int x    = x_base + lead;
                    bits &= (uint8_t)~(uint8_t)(0x80u >> lead);
                    visited++;
                    if (!func(user_data, x, y)) return visited;
                }
            }
        }
        return visited;
    }
    // Legacy Arduino stream layout: keep per-pixel semantics.
    for (int y = 0; y < bmap->height; ++y) {
        for (int x = 0; x < bmap->width; ++x) {
            if (!dgx_bw_bitmap_get_pixel(bmap, x, y)) continue;
            visited++;
            if (func && !func(user_data, x, y)) return visited;
        }
    }
    return visited;
}

bool dgx_bw_bitmap_get_pixel(dgx_bw_bitmap_t *bmap, int x, int y)
{
    int     offset;
    uint8_t bmask;
    if (!bmap->is_stream) { // DGX_FONT_BITMAP_LINES
        int pitch = (bmap->width + 7) / 8;
        offset    = y * pitch + x / 8;
        bmask     = 0x80 >> (x & 7);
    } else { // DGX_FONT_BITMAP_STREAM
        offset = y * bmap->width + x;
        bmask  = 0x80 >> (offset & 7);
        offset = (offset + 7) / 8;
    }
    return !!(bmap->bitmap[offset] & bmask);
}

void dgx_bw_bitmap_set_pixel(dgx_bw_bitmap_t *bmap, int x, int y, bool color)
{
    int     offset;
    uint8_t bmask;
    if (!bmap->is_stream) { // DGX_FONT_BITMAP_LINES
        int pitch = (bmap->width + 7) / 8;
        offset    = y * pitch + x / 8;
        bmask     = 0x80 >> (x & 7);
    } else { // DGX_FONT_BITMAP_STREAM
        offset = y * bmap->width + x;
        bmask  = 0x80 >> (offset & 7);
        offset = (offset + 7) / 8;
    }
    if (color) {
        bmap->bitmap[offset] |= bmask;
    } else {
        bmap->bitmap[offset] &= ~bmask;
    }
}

void dgx_bw_bitmap_blit_or(dgx_bw_bitmap_t *bmap_dst, int x, int y, const dgx_bw_bitmap_t *bmap_src)
{
    if (!bmap_dst || !bmap_src || bmap_dst->is_stream || bmap_src->is_stream) return;

    int bw = bmap_src->width;
    int bh = bmap_src->height;
    int x1 = x;
    int y1 = y;
    int skip_src_x = 0;
    int skip_src_y = 0;

    if (x1 < 0) {
        skip_src_x = -x1;
        bw += x1;
        x1 = 0;
    }
    if (y1 < 0) {
        skip_src_y = -y1;
        bh += y1;
        y1 = 0;
    }
    if (x1 + bw > bmap_dst->width)  bw = bmap_dst->width  - x1;
    if (y1 + bh > bmap_dst->height) bh = bmap_dst->height - y1;
    if (bw <= 0 || bh <= 0) return;

    int dst_pitch = (bmap_dst->width + 7) / 8;
    int src_pitch = (bmap_src->width + 7) / 8;
    uint8_t *dst_line = bmap_dst->bitmap + y1 * dst_pitch;
    uint8_t *src_line = bmap_src->bitmap + skip_src_y * src_pitch;

    for (int r = 0; r < bh; ++r, dst_line += dst_pitch, src_line += src_pitch) {
        int c = 0;
        while (c < bw) {
            int     src_bit = c + skip_src_x;
            uint8_t val     = src_line[src_bit >> 3];
            uint8_t ri      = (uint8_t)(src_bit & 7);
            uint8_t blen    = (uint8_t)(8 - ri);
            if (c + blen > bw) blen = (uint8_t)(bw - c);
            if (ri) val = (uint8_t)(val << ri);
            dgx_or_bits_msb(dst_line, (size_t)(x1 + c), val, blen);
            c += blen;
        }
    }
}
