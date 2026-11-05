/*
 * The windows on the computer, host/window/host_screen.c as it is, with
 * SDL's dummy driver and so no window opened: what the panel behind a bus
 * shows is what a virtual screen gets from the same drawing, at 16 and 18
 * bits and in every way the screen may lie on the glass; two windows, each
 * with its glass and its bus; the bytes of a pass and their time on the
 * wire; the memory counted and the limits of it; windows let go of.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/vscreen.h"
#include "fonts/TerminusTTFMedium12.h"
#include "host_screen.c"

#define W 64
#define H 48

/* through pointers, so that the compiler cannot leave out an allocation the test makes and does not use */
static void *(*volatile take)(size_t)            = malloc;
static void *(*volatile take_n)(size_t, size_t)  = calloc;
static void *(*volatile retake)(void *, size_t)  = realloc;
static void (*volatile give)(void *)             = free;

/* something of every kind of drawing, the same on any screen */
static void scene(dgx_screen_t *s, uint8_t bits)
{
    uint32_t (*rgb)(uint8_t, uint8_t, uint8_t) = bits == 18 ? dgx_rgb_to_18 : dgx_rgb_to_16;
    dgx_fill_rectangle(s, 0, 0, s->width, s->height, rgb(10, 20, 30));
    dgx_fill_rectangle(s, 3, 4, 20, 9, rgb(255, 0, 0));
    dgx_draw_line(s, 0, 0, s->width - 1, s->height - 1, rgb(0, 255, 0));
    dgx_draw_line_thick(s, 5, 40, 60, 10, 3, rgb(255, 255, 0));
    dgx_draw_circle(s, 30, 24, 12, rgb(0, 0, 255));
    dgx_solid_circle(s, 50, 30, 6, rgb(255, 0, 255));
    const dgx_point_2d_t curve[3] = {{2, 20}, {20, 0}, {40, 30}};
    dgx_draw_bezier3(s, curve, 0, 2, rgb(0, 255, 255));
    dgx_font_string_utf8_screen(s, 2, 46, "Ab", rgb(255, 255, 255), DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
    dgx_screen_t *sprite = dgx_vscreen_init(10, 6, bits, DgxScreenRGB);
    CHECK(sprite != NULL);
    if (sprite) {
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 10; ++x) dgx_set_pixel(sprite, x, y, rgb((uint8_t)(x * 25), (uint8_t)(y * 40), 128));
        dgx_vscreen_to_screen(s, 40, 2, sprite, 0);
        dgx_screen_destroy(&sprite);
    }
    dgx_set_pixel(s, s->width - 1, s->height - 1, rgb(255, 255, 255));
}

/* the window of a screen: the bus of the screen is the first thing in it */
static host_window_t *window_of(dgx_screen_t *s)
{
    return (host_window_t *)((dgx_screen_with_bus_t *)s)->bus;
}

/* the colors the glass and a virtual screen have kept, in as many bits as the depth has */
static uint32_t kept(uint32_t argb, uint8_t bits)
{
    return bits == 18 ? argb & 0xFCFCFCu : argb & 0xF8FCF8u;
}

/* the scene on the screen of the glass, turned so, and on a virtual screen as large as that screen: pixel by pixel the same */
static void check_scene(dgx_screen_t *host, uint8_t bits, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap)
{
    const host_window_t *w  = window_of(host);
    const int            gw = w->width, gh = w->height;
    dgx_panel_orientation(host, dir_x, dir_y, swap);
    const int sw = host->width, sh = host->height;
    CHECK(sw == (swap ? gh : gw) && sh == (swap ? gw : gh));
    scene(host, bits);
    dgx_screen_t *v = dgx_vscreen_init(sw, sh, bits, DgxScreenRGB);
    CHECK(v != NULL);
    if (!v) return;
    scene(v, bits);
    uint32_t *want = (uint32_t *)malloc(sizeof(uint32_t) * sw * sh);
    CHECK(want && dgx_vscreen_to_argb8888(v, 0, 0, sw, sh, NULL, want, sw));
    int wrong = 0;
    for (int y = 0; want && y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            /* as dgx_screen.h has it: the axes swapped first, the columns of the panel then counted from the right and its rows from the bottom */
            int gx = swap ? y : x, gy = swap ? x : y;
            if (dir_x == DgxScreenRightLeft) gx = gw - 1 - gx;
            if (dir_y == DgxScreenBottomTop) gy = gh - 1 - gy;
            if (kept(w->glass[gy * gw + gx], bits) != kept(want[y * sw + x], bits) && !wrong++)
                fprintf(stderr, "window %d, %u bits, %d %d %d: screen %d,%d glass %d,%d: %06x, a virtual screen %06x\n", w->number, bits, dir_x,
                        dir_y, swap, x, y, gx, gy, (unsigned)(w->glass[gy * gw + gx] & 0xFFFFFFu), (unsigned)(want[y * sw + x] & 0xFFFFFFu));
        }
    CHECK(!wrong);
    free(want);
    dgx_screen_destroy(&v);
}

static void check_every_way(dgx_screen_t *host, uint8_t bits)
{
    for (int way = 0; way < 8; ++way)
        check_scene(host, bits, way & 1 ? DgxScreenRightLeft : DgxScreenLeftRight, way & 2 ? DgxScreenBottomTop : DgxScreenTopBottom, way & 4);
}

/* a sum of the glass, to see that it has not changed */
static uint64_t glass_sum(const host_window_t *w)
{
    uint64_t sum = 0;
    for (int i = 0; i < w->width * w->height; ++i) sum = sum * 31 + w->glass[i];
    return sum;
}

void app_main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    dgx_screen_t *host = dgx_host_screen_init(W, H, 16, NULL);
    CHECK(host != NULL);
    if (!host) exit(EXIT_FAILURE);
    host_window_t *one = window_of(host);
    CHECK(one->number == 1 && !strcmp(one->title, "DGX"));
    CHECK(one->protocols.buffer_len == 4736); /* as the SPI bus of the CYD has it at 40 MHz */

    /* a second display, on QSPI at 20 MHz: its bus has a buffer of its own, as long as the wire takes in 950 us */
    const dgx_host_screen_options_t qspi = {.bus_hz = 20000000, .bus_lines = 4};
    dgx_screen_t                   *bar  = dgx_host_screen_init(32, 16, 16, &qspi);
    CHECK(bar != NULL);
    if (!bar) exit(EXIT_FAILURE);
    host_window_t *two = window_of(bar);
    CHECK(two != one && two->number == 2 && !strcmp(two->title, "DGX 2") && one->next == two && !two->next);
    CHECK(two->protocols.buffer_len == 9488 && two->protocols.buffer != one->protocols.buffer);
    CHECK(dgx_host_screen_init(0, 10, 16, NULL) == NULL && windows == one && one->next == two);

    /* a pass: the screen filled, from one wait to the next; the window, its command and the pixels; the other bus sends nothing */
    dgx_fill_rectangle(host, 0, 0, W, H, dgx_rgb_to_16(1, 2, 3));
    CHECK(one->bytes >= W * H * 2 && one->bytes <= W * H * 2 + 11 && two->bytes == 0);
    const uint64_t bytes = one->bytes;
    dgx_host_delay(0);
    /* both made before the first wait: put side by side, each glass the same number of times as large */
    int one_w, one_h, two_w, two_h;
    SDL_GetWindowSize(one->window, &one_w, &one_h);
    SDL_GetWindowSize(two->window, &two_w, &two_h);
    CHECK(one->placed && two->placed && one_w > W && one_w * 32 == two_w * W && one_h * 16 == two_h * H && one_w * H == one_h * W);
    dgx_host_delay(0);                 /* a wait with nothing sent is no pass */
    CHECK(one->passes == 1 && one->most_bytes == bytes && one->bytes == 0 && two->passes == 0);
    CHECK(one->most_us > bytes * 8 / 40.0 - 0.01 && one->most_us < bytes * 8 / 40.0 + 0.01);

    /* the second glass takes what is drawn on its screen, and the first does not */
    const uint64_t first = glass_sum(one);
    dgx_fill_rectangle(bar, 0, 0, 32, 16, dgx_rgb_to_16(255, 0, 0));
    CHECK(two->glass[0] == 0xFFFF0000u && two->glass[32 * 16 - 1] == 0xFFFF0000u && glass_sum(one) == first);
    dgx_host_delay(0);
    CHECK(two->passes == 1 && one->passes == 1);
    CHECK(two->most_us > two->most_bytes * 8 / 80.0 - 0.01 && two->most_us < two->most_bytes * 8 / 80.0 + 0.01);

    check_every_way(host, 16);
    check_every_way(bar, 16);

    /* 18 bits: another screen of the same panel, as a program cannot make it, with three bytes a pixel */
    dgx_screen_t *deep = dgx_panel_init(&one->protocols, &one->description, GPIO_NUM_NC, GPIO_NUM_NC, 18, DgxScreenRGB);
    CHECK(deep != NULL && one->colmod == 0x66);
    if (deep) check_every_way(deep, 18);
    CHECK(dgx_panel_init(&one->protocols, &one->description, GPIO_NUM_NC, GPIO_NUM_NC, 12, DgxScreenRGB) == NULL);

    /* the memory: what is held and let go of, the largest block, and the limits */
    const size_t before = heap.now;
    void        *p      = take(1000);
    CHECK(p && heap.now >= before + 1000 && heap.block >= 1000);
    p = retake(p, 5000);
    CHECK(p && heap.now >= before + 5000 && heap.most >= heap.now && heap.block >= 5000);
    give(p);
    CHECK(heap.now == before);
    p = take_n(10, 30);
    CHECK(p && heap.now >= before + 300);
    give(p);
    CHECK(heap.now == before);
    heap.block_limit = 500;
    CHECK(take(1000) == NULL && heap.refused == 1);
    heap.block_limit = 0;
    heap.limit       = heap.now + 2000;
    p                = take(1500);
    CHECK(p != NULL);
    CHECK(take(1500) == NULL && heap.refused == 2);
    CHECK(retake(p, 2500) == NULL && heap.refused == 3); /* p stays */
    give(p);
    CHECK(heap.now == before);
    heap.limit = 0;

    /* the screens and their windows let go of: the buffers of the buses are the program's memory, the rest is not */
    dgx_screen_destroy(&deep);
    dgx_screen_destroy(&host);
    dgx_screen_destroy(&bar);
    const size_t buffers = heap.now;
    one->protocols.dispose(&one->protocols);
    CHECK(windows == two && heap.now < buffers);
    two->protocols.dispose(&two->protocols);
    CHECK(windows == NULL && heap.now == 0);
    if (check_failures) {
        fprintf(stderr, "%s: %d check(s) failed\n", __FILE__, check_failures);
        exit(EXIT_FAILURE);
    }
    printf("%s: ok\n", __FILE__);
    exit(EXIT_SUCCESS);
}
