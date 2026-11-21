/*
 * A picture of one color, a 1-bit one or a silhouette of any depth, drawn on
 * any screen: virtual screens of every depth, the monochrome page screen and
 * a display behind a bus. What the screen holds afterwards has to be what a
 * twin of it holds where the same pixels were set one by one: every
 * orientation, clip and origin. The screen is updated once a picture, and a
 * display behind a bus gets a window for every run of pixels along a row,
 * not one for every pixel.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_bw_screen.h"
#include "dgx_draw.h"
#include "dgx_image.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"
#include "dgx_vscreen.h"

#define DW 41
#define DH 29

static uint32_t seed = 90210;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

/* where the pixel (sx, sy) of a picture w x h goes in the picture turned */
static void turned(dgx_output_orientation_t turn, int w, int h, int sx, int sy, int *dx, int *dy)
{
    switch (turn) {
    case DgxOutputNormal: *dx = sx, *dy = sy; break;
    case DgxOutputMirrorX: *dx = w - 1 - sx, *dy = sy; break;
    case DgxOutputMirrorY: *dx = sx, *dy = h - 1 - sy; break;
    case DgxOutputRotate180: *dx = w - 1 - sx, *dy = h - 1 - sy; break;
    case DgxOutputTranspose: *dx = sy, *dy = sx; break;
    case DgxOutputRotate90CCW: *dx = sy, *dy = w - 1 - sx; break;
    case DgxOutputRotate90CW: *dx = h - 1 - sy, *dy = sx; break;
    default: *dx = h - 1 - sy, *dy = w - 1 - sx; break;
    }
}

static unsigned pixel(const dgx_image_t *image, int sx, int sy)
{
    const int stride = (image->width * image->format + 7) / 8;
    const int bit    = sx * image->format;
    return (image->data[sy * stride + bit / 8] >> (8 - image->format - bit % 8)) & ((1u << image->format) - 1u);
}

/* the model: the pixels of the screen the picture puts its color on, and the box of the turned picture */
static void model_blit(bool *inked, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how, int *bl, int *bt, int *br, int *bb)
{
    int ox, oy;
    turned(how->orientation, image->width, image->height, image->origin_x, image->origin_y, &ox, &oy);
    memset(inked, 0, DW * DH * sizeof(*inked));
    *bl = *bt = 1 << 20, *br = *bb = -(1 << 20);
    for (int sy = 0; sy < image->height; ++sy) {
        for (int sx = 0; sx < image->width; ++sx) {
            int dx, dy;
            turned(how->orientation, image->width, image->height, sx, sy, &dx, &dy);
            dx += x - ox, dy += y - oy;
            if (dx < *bl) *bl = dx;
            if (dx > *br) *br = dx;
            if (dy < *bt) *bt = dy;
            if (dy > *bb) *bb = dy;
            if (!pixel(image, sx, sy) || dx < 0 || dy < 0 || dx >= DW || dy >= DH) continue;
            if ((how->flags & DGX_IMAGE_CLIP) &&
                (dx < how->clip_x || dy < how->clip_y || dx >= how->clip_x + how->clip_w || dy >= how->clip_y + how->clip_h))
                continue;
            inked[dy * DW + dx] = true;
        }
    }
}

/* the clips tried: none, inside, over each edge, outside, larger than the screen, empty */
static void pick_clip(int kind, dgx_image_blit_t *how)
{
    static const int16_t clips[][4] = {
        {5, 4, 20, 15},  {-6, 3, 15, 10}, {30, 3, 20, 10}, {4, -5, 15, 12}, {4, 20, 15, 20},
        {50, 40, 10, 10}, {-10, -10, 70, 60}, {5, 5, 0, 10}, {5, 5, 10, 0}, {-30, 5, 10, 10},
    };
    if (kind == 0) return;
    how->flags |= DGX_IMAGE_CLIP;
    how->clip_x = clips[kind - 1][0], how->clip_y = clips[kind - 1][1];
    how->clip_w = clips[kind - 1][2], how->clip_h = clips[kind - 1][3];
}
#define CLIP_KINDS 11

/*
 * A display behind a bus: a window, then the bytes of its pixels, row by row.
 * As a bus that sends by DMA it reads the bytes of a piece only when it is
 * done with it, so a piece written over before it has gone shows up as wrong
 * pixels.
 */
typedef struct {
    dgx_screen_with_bus_t sbus; /* first: the screen leads to its panel */
    dgx_bus_protocols_t   bus;
    uint8_t               buffer[48];
    uint32_t              glass[DW * DH];
    int                   bytes;
    int                   l, r, b, x, y;
    long                  windows;
    const uint8_t        *pending;
    uint32_t              pending_bits;
} panel_t;

static void panel_take(panel_t *p)
{
    if (!p->pending) return;
    for (uint32_t i = 0; i + (uint32_t)p->bytes <= p->pending_bits / 8u; i += (uint32_t)p->bytes) {
        uint32_t color = 0;
        for (int k = 0; k < p->bytes; ++k) color = color << 8 | p->pending[i + k];
        CHECK(p->y <= p->b);
        if (p->y <= p->b) p->glass[p->x + p->y * DW] = color;
        if (++p->x > p->r) p->x = p->l, ++p->y;
    }
    p->pending = NULL;
}

static void panel_set_area(dgx_screen_t *scr, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    panel_t *p = (panel_t *)scr;
    panel_take(p);
    CHECK(left <= right && top <= bottom && right < DW && bottom < DH);
    p->l = left, p->r = right, p->b = bottom, p->x = left, p->y = top;
    ++p->windows;
}

static void panel_write_area(dgx_screen_t *scr, uint8_t *data, uint32_t lenbits)
{
    panel_t *p = (panel_t *)scr;
    panel_take(p);
    CHECK(lenbits % (8u * (uint32_t)p->bytes) == 0);
    p->pending = data, p->pending_bits = lenbits;
}

static void panel_wait(dgx_screen_t *scr)
{
    panel_take((panel_t *)scr);
}

static int calls, last_l, last_r, last_t, last_b;

static void record(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr;
    ++calls;
    last_l = left, last_r = right, last_t = top, last_b = bottom;
}

static void quiet(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    (void)scr, (void)left, (void)right, (void)top, (void)bottom;
}

static dgx_screen_t *panel_screen(panel_t *p, uint8_t bits)
{
    memset(p, 0, sizeof(*p));
    p->bus.buffer = p->buffer, p->bus.buffer_len = sizeof(p->buffer);
    p->sbus.bus       = &p->bus;
    p->bytes          = bits == 16 ? 2 : 3;
    dgx_screen_t *lcd = &p->sbus.scr;
    lcd->width = DW, lcd->height = DH, lcd->color_bits = bits, lcd->screen_subtype = DgxPhysicalScreenWithBus;
    /* the rectangles and the pixels of every display behind a bus, into the window of this one */
    dgx_scr_init_slow_bus_optimized_funcs(lcd);
    lcd->set_area = panel_set_area, lcd->write_area = panel_write_area, lcd->wait = panel_wait;
    lcd->update_screen = record;
    return lcd;
}

/* the destinations: virtual screens of every depth but 8, the page screen, the 8-bit virtual screen, displays behind a bus */
enum { KIND_VSCREEN_LAST = 6, KIND_PAGES, KIND_INDEXED, KIND_BUS_16, KIND_BUS_18, KIND_BUS_24, KINDS };
static const uint8_t kind_bits[KINDS] = {1, 4, 12, 16, 18, 24, 32, 1, 8, 16, 18, 24};

static uint32_t color_mask(uint8_t bits)
{
    switch (bits) {
    case 1: return 1u;
    case 4: return 0xf0u; /* a pixel of 4 bits is kept in the high half of a byte */
    case 12: return 0xfff0u;
    case 18: return 0xfcfcfcu;
    case 32: return 0xffffffffu;
    default: return (1u << bits) - 1u;
    }
}

static uint32_t any_color(uint32_t mask)
{
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 4) & mask;
}

static void test_kind(int kind)
{
    static const int     widths[]  = {1, 7, 9, 17, 3, 17};
    static const int     heights[] = {1, 5, 11, 2};
    static const uint8_t formats[] = {DGX_IMAGE_1BPP, DGX_IMAGE_4BPP, DGX_IMAGE_8BPP};
    static panel_t       panel;
    static uint32_t      expected[DW * DH], before[DW * DH];
    static bool          inked[DW * DH];
    static uint8_t       data[17 * 11];
    const uint8_t        bits   = kind_bits[kind];
    const uint32_t       mask   = color_mask(bits);
    const bool           on_bus = kind >= KIND_BUS_16;
    dgx_screen_t        *drawn, *twin = NULL;
    if (on_bus) {
        drawn = panel_screen(&panel, bits);
    } else if (kind == KIND_PAGES) {
        drawn = dgx_bw_init(DW, DH), twin = dgx_bw_init(DW, DH);
    } else {
        drawn = dgx_vscreen_init(DW, DH, bits, DgxScreenRGB), twin = dgx_vscreen_init(DW, DH, bits, DgxScreenRGB);
    }
    CHECK(drawn && (on_bus || twin));
    if (!drawn || (!on_bus && !twin)) return;
    drawn->update_screen = record;
    if (twin) twin->update_screen = quiet;
    long rounds = 0, drawn_rounds = 0, wrong_rounds = 0, update_wrong = 0, window_wrong = 0;
    for (int f = 0; f < 3; ++f) {
        for (int turn = 0; turn < 8; ++turn) {
            for (int clip = 0; clip < CLIP_KINDS; ++clip) {
                for (int shape = 0; shape < 6; ++shape, ++rounds) {
                    dgx_image_t image = {.width = (uint16_t)widths[rnd(0, 5)], .height = (uint16_t)heights[rnd(0, 3)], .format = formats[f], .data = data};
                    /* some of its bytes empty, now and then all of them */
                    const int   dense = rnd(0, 11) ? rnd(1, 3) : 0;
                    for (size_t i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(rnd(0, dense) ? rnd(0, 255) : 0);
                    /* the origin inside the picture, on its edge or outside it */
                    const int where = shape % 3;
                    image.origin_x  = (int16_t)(where == 0 ? rnd(0, image.width - 1) : where == 1 ? image.width - 1 : rnd(-6, image.width + 6));
                    image.origin_y  = (int16_t)(where == 0 ? rnd(0, image.height - 1) : where == 1 ? 0 : rnd(-6, image.height + 6));
                    dgx_image_blit_t how = {.orientation = (dgx_output_orientation_t)turn, .color = any_color(mask)};
                    /* 4 and 8 bits go as a silhouette; 1 bit is one color with the flag and without it */
                    if (image.format != DGX_IMAGE_1BPP || rnd(0, 1)) how.flags |= DGX_IMAGE_SILHOUETTE;
                    pick_clip(clip, &how);
                    const int x = rnd(-8, DW + 8), y = rnd(-8, DH + 8);

                    /* the same background on both, then the picture on one and its pixels one by one on the other */
                    for (int i = 0; i < DW * DH; ++i) {
                        const uint32_t c = any_color(mask);
                        before[i] = expected[i] = c;
                        if (on_bus) {
                            panel.glass[i] = c;
                        } else {
                            dgx_set_pixel(drawn, i % DW, i / DW, c);
                            dgx_set_pixel(twin, i % DW, i / DW, c);
                        }
                    }
                    int bl, bt, br, bb;
                    model_blit(inked, x, y, &image, &how, &bl, &bt, &br, &bb);
                    int set = 0, runs = 0, il = DW, it = DH, ir = -1, ib = -1;
                    for (int i = 0; i < DW * DH; ++i) {
                        if (!inked[i]) continue;
                        const int px = i % DW, py = i / DW;
                        ++set;
                        if (px == 0 || !inked[i - 1]) ++runs;
                        il = DGX_MIN(il, px), ir = DGX_MAX(ir, px), it = DGX_MIN(it, py), ib = DGX_MAX(ib, py);
                        expected[i] = how.color;
                        if (twin) dgx_set_pixel(twin, px, py, how.color);
                    }
                    calls = 0, panel.windows = 0;
                    CHECK(dgx_image_blit(drawn, x, y, &image, &how) == ESP_OK);
                    if (on_bus) panel_wait(drawn);

                    int wrong = 0;
                    for (int i = 0; i < DW * DH; ++i) {
                        if (on_bus) wrong += panel.glass[i] != expected[i];
                        else wrong += dgx_get_pixel(drawn, i % DW, i / DW) != dgx_get_pixel(twin, i % DW, i / DW);
                    }
                    if (!on_bus && kind != KIND_PAGES) {
                        /* byte for byte, not only as get_pixel() sees it */
                        wrong += memcmp(((dgx_vscreen_t *)drawn)->v_array, ((dgx_vscreen_t *)twin)->v_array, dgx_color_points_to_bytes(bits, DW * DH)) != 0;
                    }
                    if (wrong) ++wrong_rounds;
                    /*
                     * One update a picture, of an area that holds every pixel put and lies within the box of the
                     * picture, the clip and the screen; none when nothing is put. The 8-bit virtual screen marks
                     * the whole box it went over, as it did before.
                     */
                    if (kind == KIND_INDEXED) {
                        if (calls > 1 || (set && !calls)) ++update_wrong;
                    } else if (calls != (set ? 1 : 0)) {
                        ++update_wrong;
                    }
                    if (calls == 1) {
                        if (set && (last_l > il || last_r < ir || last_t > it || last_b < ib)) ++update_wrong;
                        if (last_l < DGX_MAX(0, bl) || last_r > DGX_MIN(DW - 1, br) || last_t < DGX_MAX(0, bt) || last_b > DGX_MIN(DH - 1, bb)) ++update_wrong;
                        if ((how.flags & DGX_IMAGE_CLIP) &&
                            (last_l < how.clip_x || last_t < how.clip_y || last_r >= how.clip_x + how.clip_w || last_b >= how.clip_y + how.clip_h))
                            ++update_wrong;
                    }
                    /* a window a run of pixels along a row */
                    if (on_bus && panel.windows != runs) ++window_wrong;
                    drawn_rounds += set != 0;
                }
            }
        }
    }
    CHECK(wrong_rounds == 0);
    CHECK(update_wrong == 0);
    CHECK(window_wrong == 0);
    /* the clips that leave nothing are 4 of 11: still a good part of the rounds draws something */
    CHECK(drawn_rounds > rounds / 10);

    /* values and maps are indices of an 8-bit virtual screen: refused elsewhere, with nothing changed */
    static const uint8_t ones[] = {0xff, 0xff, 0xff, 0xff};
    for (int f = 1; f < 3; ++f) {
        const dgx_image_t image = {.width = 2, .height = 2, .format = formats[f], .data = ones};
        static uint8_t    map[255];
        memset(map, 7, sizeof map);
        const dgx_image_blit_t mapped = {.map = map};
        calls = 0, panel.windows = 0;
        for (int i = 0; i < DW * DH; ++i) before[i] = on_bus ? panel.glass[i] : dgx_get_pixel(drawn, i % DW, i / DW);
        const esp_err_t want = kind == KIND_INDEXED ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        CHECK(dgx_image_blit(drawn, 3, 3, &image, NULL) == want);
        CHECK(dgx_image_blit(drawn, 3, 3, &image, &mapped) == want);
        if (kind == KIND_INDEXED) continue;
        int changed = 0;
        for (int i = 0; i < DW * DH; ++i) changed += before[i] != (on_bus ? panel.glass[i] : dgx_get_pixel(drawn, i % DW, i / DW));
        CHECK(!changed && !calls && !panel.windows);
    }

    /* without `how` a 1-bit picture is drawn in the color 0 */
    {
        const dgx_image_t image = {.width = 8, .height = 2, .format = DGX_IMAGE_1BPP, .data = ones};
        if (on_bus) {
            for (int i = 0; i < DW * DH; ++i) panel.glass[i] = mask;
        } else {
            dgx_fill_rectangle(drawn, 0, 0, DW, DH, mask);
        }
        CHECK(dgx_image_blit(drawn, 4, 5, &image, NULL) == ESP_OK);
        if (on_bus) panel_wait(drawn);
        int wrong = 0;
        for (int i = 0; i < DW * DH; ++i) {
            const int      px = i % DW, py = i / DW;
            const uint32_t is = on_bus ? panel.glass[i] : dgx_get_pixel(drawn, px, py);
            wrong += is != (px >= 4 && px < 12 && py >= 5 && py < 7 ? 0u : mask);
        }
        CHECK(wrong == 0);
    }

    if (!on_bus) {
        dgx_screen_destroy(&drawn);
        dgx_screen_destroy(&twin);
    }
}

/* an icon on a 16-bit display, as a program draws it: its box first, then the bitmap in a color */
static void test_icon(void)
{
    static panel_t panel;
    /* 8 x 5: a frame with a dot in the middle */
    static const uint8_t bits[]  = {0xff, 0x81, 0x99, 0x81, 0xff};
    const dgx_image_t    icon    = {.width = 8, .height = 5, .format = DGX_IMAGE_1BPP, .data = bits};
    dgx_screen_t        *display = panel_screen(&panel, 16);
    dgx_screen_batch_begin(display);
    dgx_fill_rectangle(display, 10, 10, 8, 5, 0x0010);
    CHECK(dgx_image_blit(display, 10, 10, &icon, &(dgx_image_blit_t){.color = 0xffe0}) == ESP_OK);
    calls = 0;
    dgx_screen_batch_end(display);
    panel_wait(display);
    CHECK(calls == 1 && last_l == 10 && last_r == 17 && last_t == 10 && last_b == 14);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 8; ++x) CHECK(panel.glass[(10 + y) * DW + 10 + x] == ((bits[y] << x) & 0x80 ? 0xffe0u : 0x0010u));
    }
    /* the box in one window, then the runs: two whole rows, and two or three pieces in the rows between */
    CHECK(panel.windows == 1 + 1 + 2 + 3 + 2 + 1);
}

int main(void)
{
    for (int kind = 0; kind < KINDS; ++kind) test_kind(kind);
    test_icon();
    CHECK_DONE();
}
