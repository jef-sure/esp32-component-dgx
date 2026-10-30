/*
 * The window of a virtual screen as write_area() fills it: the pixels of whole
 * bytes go by rows of the window, and the result has to be that of one pixel
 * at a time. Every depth, against a model that does it so: windows of every
 * shape, with their bounds the wrong way round as well, cut into pieces that
 * end anywhere and go on past the end of the window, where it starts again.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "drivers/vscreen.h"
#include "vscreen/dgx_vscreen_priv.h"

#define W 23
#define H 17

static uint32_t seed = 2654435769u;

static uint32_t next(uint32_t limit)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % limit;
}

/* the next pixel of the window, whatever way its bounds go */
static void model_step(dgx_vscreen_t *m)
{
    if (m->area.x_offset >= m->area.right) {
        m->area.x_offset = m->area.left;
        m->area.y_offset = m->area.y_offset >= m->area.bottom ? m->area.top : m->area.y_offset + 1;
    } else {
        m->area.x_offset++;
    }
}

/* one pixel at a time */
static void model_write(dgx_vscreen_t *m, uint8_t *data, uint32_t lenbits)
{
    const uint8_t  bits       = m->base.color_bits;
    const uint32_t pixel_bits = bits == 18 ? 24u : bits;
    for (int idx = 0; lenbits >= pixel_bits; ++idx, lenbits -= pixel_bits) {
        uint32_t offset = DGX_VSCR_OFFSET(m, m->area.x_offset, m->area.y_offset);
        uint32_t color  = dgx_read_buf_value(bits, &data, idx);
        dgx_fill_buf_value(bits, DGX_VSCR_PTR(m, offset), offset, color);
        model_step(m);
    }
}

static void check_depth(uint8_t bits)
{
    dgx_screen_t *s = dgx_vscreen_init(W, H, bits, DgxScreenRGB);
    CHECK(s != NULL);
    if (!s) return;
    dgx_vscreen_t  *v     = (dgx_vscreen_t *)s;
    const uint32_t  size  = dgx_color_points_to_bytes(bits, W * H);
    uint8_t        *array = calloc(size, 1);
    dgx_vscreen_t   m     = *v;
    const uint32_t  pbits = bits == 18 ? 24u : bits;
    static uint8_t  data[8 * W * H * 4];
    m.v_array = array;
    CHECK(array != NULL);
    for (int round = 0; round < 4000 && array; ++round) {
        int left = (int)next(W), right = (int)next(W), top = (int)next(H), bottom = (int)next(H);
        switch (round & 3) {
        case 1: left = 0, right = W - 1, top = 0, bottom = H - 1; break; /* the whole screen */
        case 2: right = left; break;                                     /* one column */
        case 3: bottom = top; break;                                     /* one row */
        default: break;                                                  /* may well go the wrong way round */
        }
        s->set_area(s, (uint16_t)left, (uint16_t)right, (uint16_t)top, (uint16_t)bottom);
        m.area = v->area;
        const uint32_t window = (uint32_t)(abs(right - left) + 1) * (uint32_t)(abs(bottom - top) + 1);
        for (int piece = next(6) + 1; piece; --piece) {
            const uint32_t pixels = next(next(2) ? window * 3u + 2u : 9u);
            uint32_t       bytes  = (pixels * pbits + 7u) / 8u;
            for (uint32_t i = 0; i < bytes; ++i) data[i] = (uint8_t)next(256);
            /* bits past the last whole pixel are not a pixel */
            const uint32_t lenbits = pixels * pbits + next(pbits);
            uint8_t        stale   = data[(lenbits + 7u) / 8u];
            s->write_area(s, data, lenbits);
            model_write(&m, data, lenbits);
            CHECK(data[(lenbits + 7u) / 8u] == stale);
            if (memcmp(array, v->v_array, size) || m.area.x_offset != v->area.x_offset || m.area.y_offset != v->area.y_offset) {
                fprintf(stderr, "%u bits, round %d, window %d..%d x %d..%d, %u pixels: at %u,%u, the model at %u,%u\n", bits, round, left, right, top,
                        bottom, (unsigned)pixels, v->area.x_offset, v->area.y_offset, m.area.x_offset, m.area.y_offset);
                CHECK(0);
                goto done;
            }
        }
    }
done:
    free(array);
    dgx_screen_destroy(&s);
}

int main(void)
{
    static const uint8_t depths[] = {1, 4, 8, 12, 16, 18, 24, 32};
    for (size_t i = 0; i < sizeof(depths); ++i) check_depth(depths[i]);
    CHECK_DONE();
}
