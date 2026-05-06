#include <stdbool.h>
#include <string.h>

#include "dgx_bitmap.h"
#include "dgx_bits.h"

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
