/*
 * An indexed picture of 1, 4 or 8 bits drawn into an 8-bit virtual screen:
 * every depth, orientation, way of coloring and clip against a plain model
 * that takes the pixels of the picture one by one and puts each where the
 * orientation sends it, around the origin.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_image.h"
#include "drivers/vscreen.h"

#define DW 41
#define DH 29

static uint32_t seed = 4242;

static int rnd(int lo, int hi)
{
    seed = seed * 1664525u + 1013904223u;
    return lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

/* where the pixel (sx, sy) of a picture w x h goes in the picture turned, as the oriented blits of vscreen.c put it */
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

/* the model: what the blit has to leave in `model`, and the box of the turned picture */
static void model_blit(uint8_t *model, int x, int y, const dgx_image_t *image, const dgx_image_blit_t *how, int *bl, int *bt, int *br, int *bb)
{
    int ox, oy;
    turned(how->orientation, image->width, image->height, image->origin_x, image->origin_y, &ox, &oy);
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
            const unsigned p = pixel(image, sx, sy);
            if (!p || dx < 0 || dy < 0 || dx >= DW || dy >= DH) continue;
            if ((how->flags & DGX_IMAGE_CLIP) &&
                (dx < how->clip_x || dy < how->clip_y || dx >= how->clip_x + how->clip_w || dy >= how->clip_y + how->clip_h))
                continue;
            uint8_t index;
            if (image->format == DGX_IMAGE_1BPP || (how->flags & DGX_IMAGE_SILHOUETTE)) index = how->color;
            else if (how->map) index = how->map[p - 1];
            else index = (uint8_t)p;
            model[dy * DW + dx] = index;
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

static void test_against_model(void)
{
    static const int    widths[] = {1, 7, 9, 17, 3, 24};
    static const int    heights[] = {1, 5, 11, 2};
    static const uint8_t formats[] = {DGX_IMAGE_1BPP, DGX_IMAGE_4BPP, DGX_IMAGE_8BPP};
    dgx_screen_t       *dst = dgx_vscreen_init(DW, DH, 8, DgxScreenRGB);
    CHECK(dst);
    uint8_t *v = ((dgx_vscreen_t *)dst)->v_array;
    static uint8_t model[DW * DH], background[DW * DH], data[17 * 11], map[255];
    long           rounds = 0, drawn = 0, wrong_rounds = 0, dirty_wrong = 0;
    for (int f = 0; f < 3; ++f) {
        for (int turn = 0; turn < 8; ++turn) {
            for (int paint = 0; paint < 3; ++paint) { /* the values themselves, a map, a silhouette */
                for (int kind = 0; kind < CLIP_KINDS; ++kind) {
                    for (int shape = 0; shape < 16; ++shape, ++rounds) {
                        dgx_image_t image = {.width = (uint16_t)widths[rnd(0, 5)], .height = (uint16_t)heights[rnd(0, 3)], .format = formats[f], .data = data};
                        if (image.width == 24) image.width = 17;
                        for (size_t i = 0; i < sizeof data; ++i) data[i] = (uint8_t)(rnd(0, 2) ? rnd(0, 255) : 0);
                        for (size_t i = 0; i < sizeof map; ++i) map[i] = (uint8_t)rnd(0, 255);
                        /* the origin inside the picture, on its edge or outside it */
                        const int where = shape % 3;
                        image.origin_x = (int16_t)(where == 0 ? rnd(0, image.width - 1) : where == 1 ? image.width - 1 : rnd(-6, image.width + 6));
                        image.origin_y = (int16_t)(where == 0 ? rnd(0, image.height - 1) : where == 1 ? 0 : rnd(-6, image.height + 6));
                        dgx_image_blit_t how = {.orientation = (dgx_output_orientation_t)turn, .color = (uint8_t)rnd(1, 255)};
                        if (paint == 1) how.map = map;
                        if (paint == 2) how.flags |= DGX_IMAGE_SILHOUETTE, how.map = rnd(0, 1) ? map : NULL;
                        pick_clip(kind, &how);
                        const int x = rnd(-8, DW + 8), y = rnd(-8, DH + 8);

                        for (int i = 0; i < DW * DH; ++i) v[i] = model[i] = background[i] = (uint8_t)rnd(0, 255);
                        int bl, bt, br, bb;
                        model_blit(model, x, y, &image, &how, &bl, &bt, &br, &bb);
                        dgx_screen_progress_up(dst);
                        CHECK(dgx_image_blit(dst, x, y, &image, &how) == ESP_OK);
                        int left = 0, top = 0, width = 0, height = 0;
                        const bool dirty = dgx_screen_take_dirty(dst, &left, &top, &width, &height);
                        dgx_screen_progress_down(dst);

                        /* the model, pixel for pixel: also nothing outside the clip has changed */
                        int wrong = 0;
                        for (int i = 0; i < DW * DH; ++i) wrong += v[i] != model[i];
                        /* the area marked lies within the box, the clip and the screen, and holds all that has changed */
                        if (dirty) {
                            CHECK(left >= 0 && top >= 0 && left + width <= DW && top + height <= DH);
                            CHECK(left >= bl && top >= bt && left + width - 1 <= br && top + height - 1 <= bb);
                            if (how.flags & DGX_IMAGE_CLIP) {
                                CHECK(left >= how.clip_x && top >= how.clip_y && left + width <= how.clip_x + how.clip_w &&
                                      top + height <= how.clip_y + how.clip_h);
                            }
                        }
                        for (int py = 0; py < DH; ++py) {
                            for (int px = 0; px < DW; ++px) {
                                const bool inside = dirty && px >= left && py >= top && px < left + width && py < top + height;
                                if (v[py * DW + px] != background[py * DW + px] && !inside) ++dirty_wrong;
                            }
                        }
                        if (wrong) ++wrong_rounds;
                        if (!dirty && bl <= br) {
                            /* nothing marked: the box missed the screen or the clip */
                            int l = DGX_MAX(0, bl), t = DGX_MAX(0, bt), r = DGX_MIN(DW - 1, br), b = DGX_MIN(DH - 1, bb);
                            if (how.flags & DGX_IMAGE_CLIP) {
                                l = DGX_MAX(l, how.clip_x), t = DGX_MAX(t, how.clip_y);
                                r = DGX_MIN(r, how.clip_x + how.clip_w - 1), b = DGX_MIN(b, how.clip_y + how.clip_h - 1);
                            }
                            if (l <= r && t <= b) ++dirty_wrong;
                        }
                        drawn += dirty;
                    }
                }
            }
        }
    }
    CHECK(wrong_rounds == 0);
    CHECK(dirty_wrong == 0);
    /* the clips that leave nothing are 4 of 11: still a good part of the rounds draws something */
    CHECK(drawn > rounds / 10);
    dgx_screen_destroy(&dst);
}

/* the marked area is exactly the box of the picture cut by the clip and the screen, and all that changed is in it */
static void test_dirty_area(void)
{
    dgx_screen_t *dst = dgx_vscreen_init(DW, DH, 8, DgxScreenRGB);
    uint8_t      *v = ((dgx_vscreen_t *)dst)->v_array;
    static const uint8_t data[] = {0x12, 0x30, 0x45, 0x60, 0x78, 0x90, 0x80, 0x00}; /* 3 x 4 pixels of 4 bits, two bytes a row */
    const dgx_image_t    image = {.width = 3, .height = 4, .origin_x = 1, .origin_y = 3, .format = DGX_IMAGE_4BPP, .data = data};
    memset(v, 0xee, DW * DH);
    dgx_screen_progress_up(dst);
    /* turned by 90 degrees clockwise: 4 wide, 3 high; the origin (1, 3) goes to (0, 1) of the turned picture */
    const dgx_image_blit_t how = {.orientation = DgxOutputRotate90CW};
    CHECK(dgx_image_blit(dst, 10, 10, &image, &how) == ESP_OK);
    int left, top, width, height;
    CHECK(dgx_screen_take_dirty(dst, &left, &top, &width, &height));
    dgx_screen_progress_down(dst);
    CHECK(left == 10 && top == 9 && width == 4 && height == 3);
    /* the origin pixel, value 0 at (1, 3): transparent; its neighbour (0, 3), value 8, lies at (10, 9) */
    CHECK(v[9 * DW + 10] == 8 && v[10 * DW + 10] == 0xee);
    for (int py = 0; py < DH; ++py) {
        for (int px = 0; px < DW; ++px) {
            if (px < left || py < top || px >= left + width || py >= top + height) CHECK(v[py * DW + px] == 0xee);
        }
    }
    dgx_screen_destroy(&dst);
}

/* a destination that is not a linear 8-bit virtual screen is refused and left as it was; wrong arguments too */
static void test_refused(void)
{
    static const uint8_t data[] = {0xff, 0xff, 0xff, 0xff};
    const dgx_image_t    image = {.width = 2, .height = 2, .format = DGX_IMAGE_8BPP, .data = data};
    dgx_screen_t        *wide = dgx_vscreen_init(8, 8, 16, DgxScreenRGB), *narrow = dgx_vscreen_init(8, 8, 4, DgxScreenRGB);
    uint8_t      *bytes[2] = {((dgx_vscreen_t *)wide)->v_array, ((dgx_vscreen_t *)narrow)->v_array};
    const size_t  sizes[2] = {8 * 8 * 2, 8 * 8 / 2};
    static uint8_t copies[2][128];
    for (int k = 0; k < 2; ++k) {
        for (size_t i = 0; i < sizes[k]; ++i) bytes[k][i] = (uint8_t)(i * 7 + 1);
        memcpy(copies[k], bytes[k], sizes[k]);
    }
    CHECK(dgx_image_blit(wide, 1, 1, &image, NULL) == ESP_ERR_NOT_SUPPORTED);
    CHECK(dgx_image_blit(narrow, 1, 1, &image, NULL) == ESP_ERR_NOT_SUPPORTED);
    CHECK(memcmp(bytes[0], copies[0], sizes[0]) == 0 && memcmp(bytes[1], copies[1], sizes[1]) == 0);

    dgx_screen_t *dst = dgx_vscreen_init(8, 8, 8, DgxScreenRGB);
    CHECK(dgx_image_blit(NULL, 0, 0, &image, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(dgx_image_blit(dst, 0, 0, NULL, NULL) == ESP_ERR_INVALID_ARG);
    dgx_image_t two_bits = image;
    two_bits.format = 2;
    CHECK(dgx_image_blit(dst, 0, 0, &two_bits, NULL) == ESP_ERR_INVALID_ARG);
    dgx_image_t no_data = image;
    no_data.data = NULL;
    CHECK(dgx_image_blit(dst, 0, 0, &no_data, NULL) == ESP_ERR_INVALID_ARG);
    const dgx_image_blit_t odd = {.orientation = (dgx_output_orientation_t)8};
    CHECK(dgx_image_blit(dst, 0, 0, &image, &odd) == ESP_ERR_INVALID_ARG);
    /* without `how` the values are the indices */
    CHECK(dgx_image_blit(dst, 3, 4, &image, NULL) == ESP_OK);
    CHECK(dgx_get_pixel(dst, 3, 4) == 0xff && dgx_get_pixel(dst, 4, 5) == 0xff);
    dgx_screen_destroy(&wide);
    dgx_screen_destroy(&narrow);
    dgx_screen_destroy(&dst);
}

int main(void)
{
    test_against_model();
    test_dirty_area();
    test_refused();
    CHECK_DONE();
}
