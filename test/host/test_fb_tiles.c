#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bus/dgx_fb_tiles_priv.h"
#include "check.h"

/*
 * Two frame buffers that change places, and a model that is drawn into
 * every time. Whatever is drawn and read, in tiles of any size, the frame
 * buffer that is about to be shown has to be the model, and a read has to
 * see the model too. What a frame buffer does not lack must not be copied.
 */

#define MAXW  80
#define MAXH  50
#define ROOM  8
#define BYTES (MAXW * MAXH * 3)

static uint32_t seed = 1961;

static int rnd(int from, int to)
{
    seed = seed * 1664525u + 1013904223u;
    return from + (int)((seed >> 8) % (uint32_t)(to - from + 1));
}

static uint8_t frame_a[BYTES + 2 * ROOM], frame_b[BYTES + 2 * ROOM], model[BYTES];

static void put(uint8_t *fb, int width, int bytes, int x, int y, uint32_t color)
{
    uint8_t *p = fb + ((size_t)y * width + x) * bytes;
    for (int i = 0; i < bytes; ++i) p[i] = (uint8_t)(color >> (8 * i));
}

/* a copier of rectangles from one frame buffer into the other: it takes the larger ones and remembers what it was given */
static struct {
    int      width, bytes; /* of the panel */
    uint32_t least;        /* pixels it starts from */
    int      calls, taken;
    uint32_t left, top, w, rows; /* of the last one */
    uint32_t pixels;       /* of all it took */
} copier;

static bool copier_copy(void *context, uint8_t *to, const uint8_t *from, uint32_t left, uint32_t top, uint32_t width, uint32_t rows)
{
    CHECK(context == &copier);
    CHECK(width > 0 && rows > 0);
    ++copier.calls;
    copier.left = left, copier.top = top, copier.w = width, copier.rows = rows;
    if (width * rows < copier.least) return false;
    ++copier.taken;
    copier.pixels += width * rows;
    const size_t pitch = (size_t)copier.width * copier.bytes;
    for (uint32_t y = top; y < top + rows; ++y) {
        memcpy(to + y * pitch + (size_t)left * copier.bytes, from + y * pitch + (size_t)left * copier.bytes, (size_t)width * copier.bytes);
    }
    return true;
}

static void test_batches(int width, int height, int bytes, int shift_x, int shift_y, int expected_shift_x, uint32_t copier_least)
{
    dgx_fb_tiles_t tiles;
    const size_t   size = (size_t)width * height * bytes;
    memset(frame_a, 0x5a, sizeof(frame_a));
    memset(frame_b, 0x5a, sizeof(frame_b));
    memset(frame_a + ROOM, 0, size);
    memset(frame_b + ROOM, 0, size);
    memset(model, 0, size);
    CHECK(dgx_fb_tiles_init(&tiles, (uint16_t)width, (uint16_t)height, (uint8_t)bytes, (uint8_t)shift_x, (uint8_t)shift_y));
    CHECK(tiles.shift_x == expected_shift_x && tiles.shift_y == shift_y);
    CHECK(tiles.rows == (height + (1 << shift_y) - 1) >> shift_y);
    CHECK(tiles.copier == NULL);
    if (copier_least) {
        memset(&copier, 0, sizeof(copier));
        copier.width = width, copier.bytes = bytes, copier.least = copier_least;
        tiles.copier = copier_copy, tiles.copier_context = &copier;
    }
    uint8_t *hidden = frame_a + ROOM, *shown = frame_b + ROOM;
    for (int batch = 0; batch < 400; ++batch) {
        for (int ops = rnd(0, 7); ops; --ops) {
            int      x0 = rnd(0, width - 1), y0 = rnd(0, height - 1);
            int      x1 = rnd(0, 3) ? rnd(x0, x0 + 12 < width ? x0 + 12 : width - 1) : rnd(x0, width - 1);
            int      y1 = rnd(0, 3) ? rnd(y0, y0 + 12 < height ? y0 + 12 : height - 1) : rnd(y0, height - 1);
            uint32_t color = (uint32_t)rnd(1, 0xffffff);
            switch (rnd(0, 7)) {
            case 0: /* a pixel */
                dgx_fb_tiles_pixel(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0);
                put(hidden, width, bytes, x0, y0, color);
                put(model, width, bytes, x0, y0, color);
                break;
            case 1: /* a rectangle written whole */
                dgx_fb_tiles_ready(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, true, true);
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        put(hidden, width, bytes, x, y, color + (uint32_t)x);
                        put(model, width, bytes, x, y, color + (uint32_t)x);
                    }
                }
                break;
            case 2: /* a rectangle with only some pixels written */
                dgx_fb_tiles_ready(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, false, true);
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        if (rnd(0, 2)) continue;
                        put(hidden, width, bytes, x, y, color + (uint32_t)y);
                        put(model, width, bytes, x, y, color + (uint32_t)y);
                    }
                }
                break;
            case 7: /* a pixel read */
                dgx_fb_tiles_pixel_read(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0);
                CHECK(memcmp(hidden + ((size_t)y0 * width + x0) * bytes, model + ((size_t)y0 * width + x0) * bytes, (size_t)bytes) == 0);
                break;
            case 3: /* a rectangle read */
                dgx_fb_tiles_ready(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, false, false);
                for (int y = y0; y <= y1; ++y) {
                    CHECK(memcmp(hidden + ((size_t)y * width + x0) * bytes, model + ((size_t)y * width + x0) * bytes, (size_t)(x1 - x0 + 1) * bytes) == 0);
                }
                break;
            case 5:
            case 6: { /* a window pixels are sent into, run after run, to its end or not; the runs go along the rows or the columns, either way */
                int swap = rnd(0, 1), back_u = rnd(0, 1), back_v = rnd(0, 1);
                int w = x1 - x0 + 1, h = y1 - y0 + 1;
                int run = swap ? h : w, runs = swap ? w : h;
                int sent = rnd(0, 2) ? run * runs : rnd(0, run * runs);
#define AT_X(u, v) (x0 + (swap ? (back_v ? runs - 1 - (v) : (v)) : (back_u ? run - 1 - (u) : (u))))
#define AT_Y(u, v) (y0 + (swap ? (back_u ? run - 1 - (u) : (u)) : (back_v ? runs - 1 - (v) : (v))))
#define SPARE(u0, v0, u1, v1)                                                                                                                  \
    do {                                                                                                                                       \
        int ax = AT_X(u0, v0), ay = AT_Y(u0, v0), bx = AT_X(u1, v1), by = AT_Y(u1, v1);                                                        \
        dgx_fb_tiles_unwritten(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, (uint16_t)(ax < bx ? ax : bx),   \
                               (uint16_t)(ay < by ? ay : by), (uint16_t)(ax < bx ? bx : ax), (uint16_t)(ay < by ? by : ay));                   \
    } while (0)
                dgx_fb_tiles_open(&tiles, hidden, shown, (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
                for (int i = 0; i < sent; ++i) {
                    put(hidden, width, bytes, AT_X(i % run, i / run), AT_Y(i % run, i / run), color + (uint32_t)i);
                    put(model, width, bytes, AT_X(i % run, i / run), AT_Y(i % run, i / run), color + (uint32_t)i);
                }
                int full = sent / run, rest = sent % run;
                if (rest) {
                    SPARE(rest, full, run - 1, full);
                    ++full;
                }
                if (full < runs) SPARE(0, full, run - 1, runs - 1);
                dgx_fb_tiles_close(&tiles, (uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
#undef AT_X
#undef AT_Y
#undef SPARE
                break;
            }
            default: /* the whole panel written */
                if (rnd(0, 5)) break;
                dgx_fb_tiles_ready(&tiles, hidden, shown, 0, 0, (uint16_t)(width - 1), (uint16_t)(height - 1), true, true);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        put(hidden, width, bytes, x, y, color ^ (uint32_t)(x * y));
                        put(model, width, bytes, x, y, color ^ (uint32_t)(x * y));
                    }
                }
                break;
            }
        }
        int taken_before = copier.taken;
        dgx_fb_tiles_finish(&tiles, hidden, shown);
        CHECK(memcmp(hidden, model, size) == 0);
        /* the copier is asked only where the frame buffers change places */
        if (copier_least) CHECK(copier.taken >= taken_before);
        /* what the copies are said to have gone into lies in the frame buffer */
        uint32_t from, length;
        if (dgx_fb_tiles_take_copied(&tiles, &from, &length)) CHECK(length > 0 && (size_t)from + length <= size);
        CHECK(!dgx_fb_tiles_take_copied(&tiles, &from, &length));
        uint8_t *other = hidden;
        hidden         = shown;
        shown          = other;
    }
    if (copier_least) CHECK(copier.taken > 0 && copier.taken < copier.calls);
    /* nothing outside the frame buffers is touched */
    for (int i = 0; i < ROOM; ++i) {
        CHECK(frame_a[i] == 0x5a && frame_a[ROOM + size + i] == 0x5a);
        CHECK(frame_b[i] == 0x5a && frame_b[ROOM + size + i] == 0x5a);
    }
    dgx_fb_tiles_free(&tiles);
    CHECK(tiles.owed == NULL && tiles.fresh == NULL);
}

/* a pixel drawn in the tile the last one was drawn in costs no look at the maps; that must not outlive a change of the frame buffers */
static void test_last_tile(void)
{
    enum { W = 64, H = 40, B = 2 };
    dgx_fb_tiles_t tiles;
    const size_t   size = (size_t)W * H * B;
    uint8_t       *a = frame_a + ROOM, *b = frame_b + ROOM;
    memset(a, 0, size);
    memset(b, 0, size);
    CHECK(dgx_fb_tiles_init(&tiles, W, H, B, 3, 3));
    CHECK(tiles.last == DGX_FB_TILES_NO_TILE);
    for (int round = 0; round < 6; ++round) {
        uint8_t *hidden = round & 1 ? b : a, *shown = round & 1 ? a : b;
        /* the same tile every time, and the tile (0,0), whose number is 0 */
        dgx_fb_tiles_pixel(&tiles, hidden, shown, (uint16_t)(40 + round), 20);
        put(hidden, W, B, 40 + round, 20, 0x1111u * (uint32_t)(round + 1));
        dgx_fb_tiles_pixel(&tiles, hidden, shown, (uint16_t)(41 + round), 21);
        put(hidden, W, B, 41 + round, 21, 0x0101u * (uint32_t)(round + 1));
        if (round > 2) {
            dgx_fb_tiles_pixel(&tiles, hidden, shown, 1, 1);
            put(hidden, W, B, 1, 1, 0x2222u * (uint32_t)(round + 1));
        }
        CHECK(tiles.last != DGX_FB_TILES_NO_TILE);
        dgx_fb_tiles_finish(&tiles, hidden, shown);
        CHECK(tiles.last == DGX_FB_TILES_NO_TILE);
        /* the one that is shown now has all the other one had, and what was drawn */
        memcpy(model, shown, size);
        put(model, W, B, 40 + round, 20, 0x1111u * (uint32_t)(round + 1));
        put(model, W, B, 41 + round, 21, 0x0101u * (uint32_t)(round + 1));
        if (round > 2) put(model, W, B, 1, 1, 0x2222u * (uint32_t)(round + 1));
        CHECK(memcmp(hidden, model, size) == 0);
    }
    dgx_fb_tiles_free(&tiles);
}

/* how many bytes of the frame buffer differ from 0 */
static size_t marked(const uint8_t *fb, size_t size)
{
    size_t count = 0;
    for (size_t i = 0; i < size; ++i) count += fb[i] != 0;
    return count;
}

/* only what is lacked is copied, and no more than the tiles it lies in */
static void test_economy(void)
{
    enum { W = 64, H = 40, B = 2, S = 3, T = 1 << S };
    dgx_fb_tiles_t tiles;
    const size_t   size = (size_t)W * H * B;
    uint8_t       *a = frame_a + ROOM, *b = frame_b + ROOM;
    uint32_t       from, length;
    memset(a, 0, size);
    memset(b, 0, size);
    CHECK(dgx_fb_tiles_init(&tiles, W, H, B, S, S));
    CHECK(tiles.shift_x == S && tiles.shift_y == S && tiles.rows == 5);

    /* a pixel drawn into `a`, then the frame buffers change places */
    dgx_fb_tiles_pixel(&tiles, a, b, 19, 10);
    put(a, W, B, 19, 10, 0x1234);
    dgx_fb_tiles_finish(&tiles, a, b);
    CHECK(!dgx_fb_tiles_take_copied(&tiles, &from, &length)); /* nothing was lacked */
    /* `b` lacks one tile. Marks all over `a` tell what is copied: only that tile may come over */
    memset(a, 0x77, size);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == (size_t)T * T * B);
    for (int y = 8; y < 16; ++y) CHECK(marked(b + ((size_t)y * W + 16) * B, (size_t)T * B) == (size_t)T * B);
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));
    CHECK(from == (8u * W + 16u) * B && from + length == (15u * W + 24u) * B);
    /* and nothing is lacked after that */
    memset(b, 0, size);
    dgx_fb_tiles_finish(&tiles, a, b);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == 0);

    /* a line from corner to corner takes the tiles it goes through, not the whole panel */
    memset(a, 0, size);
    memset(b, 0, size);
    for (int x = 0; x < W; ++x) dgx_fb_tiles_pixel(&tiles, a, b, (uint16_t)x, (uint16_t)(x * (H - 1) / (W - 1)));
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(a, 0x77, size);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) >= (size_t)W * B && marked(b, size) <= 14u * T * T * B);
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));

    /* of a tile lacked that is then drawn in by a rectangle written whole only what lies outside the rectangle is copied */
    memset(a, 0, size);
    memset(b, 0, size);
    dgx_fb_tiles_ready(&tiles, a, b, 0, 0, W - 1, H - 1, true, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(a, 0x77, size);
    dgx_fb_tiles_ready(&tiles, b, a, 0, 0, W - 1, H - 1, true, true); /* the whole panel again */
    CHECK(marked(b, size) == 0);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == 0);
    CHECK(!dgx_fb_tiles_take_copied(&tiles, &from, &length));
    /* now `a` lacks everything. A rectangle from (10,9) to (41,30), 32 by 22 pixels */
    memset(a, 0, size);
    memset(b, 0x77, size);
    dgx_fb_tiles_ready(&tiles, a, b, 10, 9, 41, 30, true, true);
    /* its tiles: columns 1..5, rows 1..3, that is 40 by 24 pixels; what of them is not the rectangle comes over */
    CHECK(marked(a, size) == (size_t)(40 * 24 - 32 * 22) * B);
    CHECK(marked(a + ((size_t)8 * W + 8) * B, (size_t)40 * B) == (size_t)40 * B);  /* the row above it */
    CHECK(marked(a + ((size_t)31 * W + 8) * B, (size_t)40 * B) == (size_t)40 * B); /* the row below */
    CHECK(marked(a + ((size_t)16 * W + 8) * B, (size_t)40 * B) == (size_t)(2 + 6) * B); /* two pixels to the left, six to the right */
    CHECK(marked(a + ((size_t)16 * W + 10) * B, (size_t)32 * B) == 0);
    CHECK(marked(a, (size_t)8 * W * B) == 0 && marked(a + (size_t)32 * W * B, (size_t)8 * W * B) == 0); /* and nothing of other tiles */
    /* the same rectangle, not written whole: nothing is left to copy, the tiles are not lacked any more */
    dgx_fb_tiles_ready(&tiles, a, b, 10, 9, 41, 30, false, true);
    CHECK(marked(a, size) == (size_t)(40 * 24 - 32 * 22) * B);
    /* a read brings a tile over as well, with the tiles lacked side by side with it: the last row of tiles; it does not count as drawing */
    dgx_fb_tiles_ready(&tiles, a, b, 60, 38, 60, 38, false, false);
    CHECK(marked(a, size) == (size_t)(40 * 24 - 32 * 22 + W * T) * B);
    CHECK(marked(a + (size_t)32 * W * B, (size_t)W * T * B) == (size_t)W * T * B);
    dgx_fb_tiles_finish(&tiles, a, b);
    CHECK(marked(a, size) == size - (size_t)(32 * 22) * B); /* all but the rectangle that was to be written whole */
    /* `b` lacks the 15 tiles drawn in, and not the one that was only read */
    memset(b, 0, size);
    memset(a, 0x77, size);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == (size_t)15 * T * T * B);
    /* a window that is filled costs no copy, though `b` lacks all of it */
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));
    dgx_fb_tiles_ready(&tiles, a, b, 0, 0, W - 1, H - 1, false, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(b, 0, size);
    memset(a, 0x77, size);
    dgx_fb_tiles_open(&tiles, b, a, 0, 0, W - 1, H - 1);
    dgx_fb_tiles_close(&tiles, 0, 0, W - 1, H - 1);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == 0);
    CHECK(!dgx_fb_tiles_take_copied(&tiles, &from, &length));
    /* a window left at the pixel (10,20): the rest of that row and the rows below come from the other frame buffer, and no more */
    memset(a, 0, size);
    memset(b, 0x77, size);
    dgx_fb_tiles_open(&tiles, a, b, 0, 0, W - 1, H - 1);
    CHECK(marked(a, size) == 0);
    dgx_fb_tiles_unwritten(&tiles, a, b, 0, 0, W - 1, H - 1, 10, 20, W - 1, 20);
    CHECK(marked(a, size) == (size_t)(W - 10) * B && marked(a + ((size_t)20 * W + 10) * B, (size_t)(W - 10) * B) == (size_t)(W - 10) * B);
    dgx_fb_tiles_unwritten(&tiles, a, b, 0, 0, W - 1, H - 1, 0, 21, W - 1, H - 1);
    CHECK(marked(a, size) == (size_t)(W - 10 + W * (H - 21)) * B && marked(a, (size_t)(20 * W + 10) * B) == 0);
    dgx_fb_tiles_close(&tiles, 0, 0, W - 1, H - 1);
    dgx_fb_tiles_finish(&tiles, a, b);
    CHECK(marked(a, size) == (size_t)(W - 10 + W * (H - 21)) * B);
    /* a window in the middle: what its tiles have outside it comes over at once, what they have in it waits */
    memset(a, 0x77, size);
    memset(b, 0, size);
    dgx_fb_tiles_open(&tiles, b, a, 10, 9, 41, 30);
    CHECK(marked(b, size) == (size_t)(40 * 24 - 32 * 22) * B);
    dgx_fb_tiles_unwritten(&tiles, b, a, 10, 9, 41, 30, 10, 12, 41, 12); /* a row of it got no pixels */
    CHECK(marked(b, size) == (size_t)(40 * 24 - 32 * 22 + 32) * B);
    dgx_fb_tiles_unwritten(&tiles, b, a, 10, 9, 41, 30, 10, 9, 41, 30); /* nothing was sent at all */
    CHECK(marked(b, size) == (size_t)15 * T * T * B);
    dgx_fb_tiles_close(&tiles, 10, 9, 41, 30);
    dgx_fb_tiles_finish(&tiles, b, a);
    dgx_fb_tiles_take_copied(&tiles, &from, &length);
    dgx_fb_tiles_finish(&tiles, a, b);

    /* the range the copies went into grows both ways, whatever the order they are made in */
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));
    dgx_fb_tiles_ready(&tiles, b, a, 10, 9, 41, 30, false, true);
    dgx_fb_tiles_finish(&tiles, b, a); /* `a` lacks 15 tiles again: the columns 1..5 of the rows 1..3 */
    CHECK(!dgx_fb_tiles_take_copied(&tiles, &from, &length));
    memset(a, 0, size);
    memset(b, 0x77, size);
    dgx_fb_tiles_ready(&tiles, a, b, 20, 20, 20, 20, false, false); /* the five tiles of the row of tiles 2 */
    CHECK(marked(a, size) == (size_t)40 * T * B && marked(a + ((size_t)16 * W + 8) * B, (size_t)40 * B) == (size_t)40 * B);
    dgx_fb_tiles_ready(&tiles, a, b, 30, 17, 30, 17, false, false); /* they are there */
    dgx_fb_tiles_ready(&tiles, a, b, 9, 9, 9, 9, false, false);     /* the row of tiles above */
    dgx_fb_tiles_ready(&tiles, a, b, 47, 31, 47, 31, false, false); /* and the one below */
    CHECK(marked(a, size) == (size_t)15 * T * T * B);
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));
    CHECK(from == (8u * W + 8u) * B && from + length == (31u * W + 48u) * B);
    /* tiles lacked that do not lie side by side with the one touched stay where they are */
    dgx_fb_tiles_pixel(&tiles, a, b, 3, 3);
    dgx_fb_tiles_pixel(&tiles, a, b, 12, 3);
    dgx_fb_tiles_pixel(&tiles, a, b, 30, 3);
    dgx_fb_tiles_pixel(&tiles, a, b, 60, 3);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(b, 0, size);
    memset(a, 0x77, size);
    dgx_fb_tiles_ready(&tiles, b, a, 4, 4, 4, 4, false, false); /* the tiles 0 and 1 of the first row of tiles */
    CHECK(marked(b, size) == (size_t)2 * T * T * B && marked(b, (size_t)16 * B) == (size_t)16 * B);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == (size_t)4 * T * T * B);
    dgx_fb_tiles_take_copied(&tiles, &from, &length);
    dgx_fb_tiles_free(&tiles);

    /* the last tiles of a row and of a column are what is left of the panel, and count as whole when written to the edge */
    enum { W2 = 37, H2 = 21 };
    const size_t size2 = (size_t)W2 * H2 * 3;
    memset(a, 0, size2);
    memset(b, 0, size2);
    CHECK(dgx_fb_tiles_init(&tiles, W2, H2, 3, 3, 3));
    CHECK(tiles.rows == 3);
    dgx_fb_tiles_ready(&tiles, a, b, 0, 0, W2 - 1, H2 - 1, false, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(a, 0x77, size2);
    dgx_fb_tiles_ready(&tiles, b, a, 32, 16, W2 - 1, H2 - 1, true, true); /* the corner tile, 5 by 5 pixels, whole */
    CHECK(marked(b, size2) == 0);
    dgx_fb_tiles_ready(&tiles, b, a, 24, 8, W2 - 2, H2 - 2, true, true); /* a pixel short of the edges */
    CHECK(marked(b, size2) == (size_t)(8 + 8) * 3); /* the last column of the tile (4,1), 8 high, and the last row of the tile (3,2), 8 wide */
    dgx_fb_tiles_finish(&tiles, b, a);
    /* all but what the two rectangles were to cover: four tiles, 13 by 13 pixels, less the 16 that came over */
    CHECK(marked(b, size2) == size2 - (size_t)(13 * 13 - 16) * 3);
    dgx_fb_tiles_free(&tiles);

    /* tiles that are wider than high: 16 by 4 pixels */
    CHECK(dgx_fb_tiles_init(&tiles, W, H, B, 4, 2));
    CHECK(tiles.shift_x == 4 && tiles.shift_y == 2 && tiles.rows == 10);
    memset(a, 0, size);
    memset(b, 0, size);
    dgx_fb_tiles_pixel(&tiles, a, b, 37, 22);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(a, 0x77, size);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(marked(b, size) == (size_t)16 * 4 * B);
    for (int y = 20; y < 24; ++y) CHECK(marked(b + ((size_t)y * W + 32) * B, (size_t)16 * B) == (size_t)16 * B);
    dgx_fb_tiles_free(&tiles);
}

/* what is left before the frame buffers change places goes as rectangles, tiles one under another together */
static void test_rest(void)
{
    enum { W = 64, H = 44, B = 2, T = 8 }; /* 8 tiles a row, 5 rows of tiles and a half */
    dgx_fb_tiles_t tiles;
    const size_t   size = (size_t)W * H * B;
    uint8_t       *a = frame_a + ROOM, *b = frame_b + ROOM;
    uint32_t       from, length;
    memset(a, 0, size);
    memset(b, 0, size);
    CHECK(dgx_fb_tiles_init(&tiles, W, H, B, 3, 3));
    CHECK(tiles.rows == 6);
    memset(&copier, 0, sizeof(copier));
    copier.width = W, copier.bytes = B, copier.least = 1;
    tiles.copier = copier_copy, tiles.copier_context = &copier;

    /* a column of the tiles 2 and 3 from the first row of tiles to the last, which is 4 pixels high */
    dgx_fb_tiles_ready(&tiles, a, b, 16, 0, 31, H - 1, false, true);
    CHECK(copier.calls == 0); /* nothing was lacked, and a touch does not ask the copier */
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(a, 0x77, size);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(copier.calls == 1 && copier.taken == 1);
    CHECK(copier.left == 16 && copier.top == 0 && copier.w == 16 && copier.rows == H);
    CHECK(marked(b, size) == (size_t)16 * H * B);
    CHECK(!dgx_fb_tiles_take_copied(&tiles, &from, &length)); /* the processor copied nothing */

    /* a touch of a tile lacked is not for the copier, though it would take it */
    memset(&copier, 0, sizeof(copier));
    copier.width = W, copier.bytes = B, copier.least = 1;
    dgx_fb_tiles_finish(&tiles, a, b);
    dgx_fb_tiles_ready(&tiles, a, b, 0, 0, W - 1, 15, false, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    dgx_fb_tiles_ready(&tiles, b, a, 3, 3, 3, 3, false, false);
    CHECK(copier.calls == 0);
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(copier.calls == 1 && copier.left == 0 && copier.top == 8 && copier.w == W && copier.rows == 8);
    dgx_fb_tiles_finish(&tiles, a, b);

    /*
     * Tiles lacked, by rows of tiles:  0: ..XX....   The run 2..3 of the row 0 does not go down: in the row 1 it is
     *                                  1: .XXXX...   a part of a longer one, which is left whole and takes the row 2 along.
     *                                  2: .XXXX...   The tile 6 of the rows 2..4 is one rectangle; the row 3 has it and
     *                                  3: X.....X.   the tile 0, each on its own.
     *                                  4: ......X.
     */
    memset(a, 0, size);
    memset(b, 0, size);
    dgx_fb_tiles_ready(&tiles, a, b, 16, 0, 31, 7, false, true);
    dgx_fb_tiles_ready(&tiles, a, b, 8, 8, 39, 23, false, true);
    dgx_fb_tiles_ready(&tiles, a, b, 48, 16, 55, 39, false, true);
    dgx_fb_tiles_ready(&tiles, a, b, 0, 24, 7, 31, false, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(a, 0x77, size);
    memset(&copier, 0, sizeof(copier));
    copier.width = W, copier.bytes = B, copier.least = 16 * 8 + 1; /* more than two tiles */
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(copier.calls == 4);
    /* taken: the four tiles by two rows, 32x16, and the tile 6 by three rows, 8x24; the two tiles of the row 0 and the tile 0 are left to the processor */
    CHECK(copier.taken == 2 && copier.pixels == 32 * 16 + 8 * 24);
    CHECK(marked(b, size) == (size_t)(16 * 8 + 32 * 16 + 8 * 24 + 8 * 8) * B);
    CHECK(dgx_fb_tiles_take_copied(&tiles, &from, &length));
    CHECK(from == 16u * B && from + length == (31u * W + 8u) * B); /* from the tiles of the row 0 to the tile 0 of the row 3 */
    /*
     * A run that is longer below to one side only is left alone as well:  0: ..XX....   3: ..XX....
     *                                                                     1: .XXX....   4: ..XXX...
     */
    dgx_fb_tiles_finish(&tiles, a, b);
    dgx_fb_tiles_take_copied(&tiles, &from, &length);
    dgx_fb_tiles_ready(&tiles, a, b, 16, 0, 31, 7, false, true);
    dgx_fb_tiles_ready(&tiles, a, b, 8, 8, 31, 15, false, true);
    dgx_fb_tiles_ready(&tiles, a, b, 16, 24, 31, 31, false, true);
    dgx_fb_tiles_ready(&tiles, a, b, 16, 32, 39, 39, false, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    memset(&copier, 0, sizeof(copier));
    copier.width = W, copier.bytes = B, copier.least = 24 * 8; /* three tiles */
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(copier.calls == 4 && copier.taken == 2 && copier.pixels == 2 * 24 * 8);
    dgx_fb_tiles_free(&tiles);

    /* a row of 32 tiles lacked whole, and the same row below: one rectangle as wide as the panel */
    memset(a, 0, (size_t)64 * 8 * 2);
    memset(b, 0, (size_t)64 * 8 * 2);
    CHECK(dgx_fb_tiles_init(&tiles, 64, 8, 2, 1, 2));
    CHECK(tiles.shift_x == 1 && tiles.rows == 2);
    memset(&copier, 0, sizeof(copier));
    copier.width = 64, copier.bytes = 2, copier.least = 1;
    tiles.copier = copier_copy, tiles.copier_context = &copier;
    dgx_fb_tiles_ready(&tiles, a, b, 0, 0, 63, 7, true, true);
    dgx_fb_tiles_finish(&tiles, a, b);
    dgx_fb_tiles_finish(&tiles, b, a);
    CHECK(copier.calls == 1 && copier.left == 0 && copier.top == 0 && copier.w == 64 && copier.rows == 8);
    dgx_fb_tiles_free(&tiles);
}

int main(void)
{
    test_batches(70, 45, 2, 2, 2, 2, 0);  /* 18 tiles a row */
    test_batches(70, 45, 3, 3, 3, 3, 0);
    test_batches(64, 32, 2, 1, 1, 1, 0);  /* 32 tiles a row: a word to the last bit */
    test_batches(37, 29, 3, 0, 0, 1, 0);  /* a tile a pixel wide would make 37 a row: it grows */
    test_batches(33, 20, 2, 0, 1, 1, 0);  /* and 33 a row, one more than a word has bits */
    test_batches(80, 50, 2, 0, 2, 2, 0);
    test_batches(33, 9, 2, 5, 5, 5, 0);   /* two tiles, the second a pixel wide */
    test_batches(80, 50, 2, 3, 1, 3, 0);  /* tiles wider than high */
    test_batches(60, 50, 3, 1, 4, 1, 0);  /* and higher than wide */
    /* the same with a copier that takes what is left when it is large enough */
    test_batches(70, 45, 2, 2, 2, 2, 40);
    test_batches(64, 32, 2, 1, 1, 1, 12);
    test_batches(37, 29, 3, 0, 0, 1, 6);
    test_batches(80, 50, 2, 3, 1, 3, 100);
    test_economy();
    test_rest();
    test_last_tile();
    CHECK_DONE();
}
