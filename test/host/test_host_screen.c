/*
 * The windows on the computer, host/window/host_screen.c as it is, with
 * SDL's dummy driver and so no window opened: what the panel behind a bus
 * shows is what a virtual screen gets from the same drawing, at 16 and 18
 * bits and in every way the screen may lie on the glass, with the scroll of
 * the panel and windows it cuts; two windows, each with its glass and its
 * bus, SPI and QSPI; what a pass costs on the wire of the board and in
 * transfers; the glass turned as the board is held; a window shown with no
 * wait; the mouse as a finger on the glass; the keys; a script of keys and
 * the mouse; a display on no bus; the memory counted, its limits and what
 * the program is told of it; windows let go of.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* the color of a depth */
static uint32_t rgb_of(uint8_t bits, uint8_t r, uint8_t g, uint8_t b)
{
    return bits == 18 ? dgx_rgb_to_18(r, g, b) : dgx_rgb_to_16(r, g, b);
}

/*
 * A band moved by the scroll of the panel, one way and then the other: from
 * edge to edge across the rows of its memory, which are the rows of the
 * screen, or its columns when the axes are swapped. Then something drawn
 * across the band, in windows the scroll cuts.
 */
static void band(dgx_screen_t *s, uint8_t bits, bool swap)
{
    const int along = swap ? s->width : s->height, first = 2, count = along - 4;
    for (int move = 3; move >= -5; move -= 8) {
        const esp_err_t rc = swap ? dgx_scroll(s, first, 0, count, s->height, move, 0, rgb_of(bits, 40, 0, 80))
                                  : dgx_scroll(s, 0, first, s->width, count, 0, move, rgb_of(bits, 40, 0, 80));
        CHECK(rc == ESP_OK);
    }
    dgx_fill_rectangle(s, 3, 3, s->width / 2, s->height / 2, rgb_of(bits, 0, 128, 255));
    dgx_draw_line(s, s->width - 1, 0, 0, s->height - 1, rgb_of(bits, 255, 128, 0));
    dgx_font_string_utf8_screen(s, 4, 14, "Cut", rgb_of(bits, 255, 255, 255), DgxOutputNormal, 1, TerminusTTFMedium12(), NULL, NULL);
}

/* what the panel shows against a virtual screen as large as the screen on it, the screen lying so on the glass */
static void compare(host_window_t *w, dgx_screen_t *v, uint8_t bits, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap,
                    const char *what)
{
    const int gw = w->width, gh = w->height, sw = v->width, sh = v->height;
    uint32_t *want = (uint32_t *)malloc(sizeof(uint32_t) * sw * sh);
    CHECK(want && dgx_vscreen_to_argb8888(v, 0, 0, sw, sh, NULL, want, sw));
    const uint32_t *glass = visible(w);
    int             wrong = 0;
    for (int y = 0; want && y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            /* as dgx_screen.h has it: the axes swapped first, the columns of the panel then counted from the right and its rows from the bottom */
            int gx = swap ? y : x, gy = swap ? x : y;
            if (dir_x == DgxScreenRightLeft) gx = gw - 1 - gx;
            if (dir_y == DgxScreenBottomTop) gy = gh - 1 - gy;
            if (kept(glass[gy * gw + gx], bits) != kept(want[y * sw + x], bits) && !wrong++)
                fprintf(stderr, "window %d, %s, %u bits, %d %d %d: screen %d,%d glass %d,%d: %06x, a virtual screen %06x\n", w->number, what, bits,
                        dir_x, dir_y, swap, x, y, gx, gy, (unsigned)(glass[gy * gw + gx] & 0xFFFFFFu), (unsigned)(want[y * sw + x] & 0xFFFFFFu));
        }
    CHECK(!wrong);
    free(want);
}

/* the scene on the screen of the glass, turned so, and on a virtual screen as large as that screen; then a band scrolled on both */
static void check_scene(dgx_screen_t *host, uint8_t bits, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap)
{
    host_window_t *w  = window_of(host);
    const int      gw = w->width, gh = w->height;
    dgx_panel_orientation(host, dir_x, dir_y, swap);
    const int sw = host->width, sh = host->height;
    CHECK(sw == (swap ? gh : gw) && sh == (swap ? gw : gh));
    dgx_screen_t *v = dgx_vscreen_init(sw, sh, bits, DgxScreenRGB);
    CHECK(v != NULL);
    if (!v) return;
    scene(host, bits);
    scene(v, bits);
    compare(w, v, bits, dir_x, dir_y, swap, "the scene");
    band(host, bits, swap);
    band(v, bits, swap);
    CHECK(visible(w) != w->glass); /* the panel has scrolled, and moved no pixel */
    compare(w, v, bits, dir_x, dir_y, swap, "a band scrolled");
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

static bool near(double a, double b)
{
    return fabs(a - b) < 0.01;
}

/* the corners of a glass seen as the board is held: where its top-left, top-right and bottom-left pixels go */
static void check_turn(host_window_t *w, int turn, int tl_x, int tl_y, int tr_x, int tr_y, int bl_x, int bl_y)
{
    const int keep = w->turn, gw = w->width, gh = w->height;
    w->turn        = turn;
    const int tw = turned_width(w), th = turned_height(w);
    CHECK(tw == (turn % 180 ? gh : gw) && th == (turn % 180 ? gw : gh));
    uint32_t *glass = (uint32_t *)calloc((size_t)gw * gh, sizeof(uint32_t)), *seen = (uint32_t *)calloc((size_t)gw * gh, sizeof(uint32_t));
    CHECK(glass && seen);
    if (glass && seen) {
        glass[0] = 1, glass[gw - 1] = 2, glass[(gh - 1) * gw] = 3;
        turn_glass(w, glass, seen);
        CHECK(seen[tl_y * tw + tl_x] == 1 && seen[tr_y * tw + tr_x] == 2 && seen[bl_y * tw + bl_x] == 3);
    }
    free(glass);
    free(seen);
    w->turn = keep;
}

/* the mouse on a window, at pixels of the window, as SDL would give it */
static void mouse(const host_window_t *w, Uint32 type, int x, int y)
{
    SDL_Event e = {0};
    e.type      = type;
    if (type == SDL_MOUSEMOTION) {
        e.motion.windowID = w->id, e.motion.x = x, e.motion.y = y, e.motion.state = SDL_BUTTON_LMASK;
    } else {
        e.button.windowID = w->id, e.button.button = SDL_BUTTON_LEFT, e.button.x = x, e.button.y = y, e.button.clicks = 1;
        e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
    }
    CHECK(SDL_PushEvent(&e) == 1);
}

/* a key on a window, as SDL would give it */
static void key(const host_window_t *w, Uint32 type, SDL_Scancode code, int repeat)
{
    SDL_Event e = {0};
    e.type = type, e.key.windowID = w->id, e.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED, e.key.repeat = (Uint8)repeat;
    e.key.keysym.scancode = code;
    CHECK(SDL_PushEvent(&e) == 1);
}

/* the keys held, as many as fit in 8 */
static int held(uint16_t keys[8])
{
    return dgx_host_keys(keys, 8);
}

/* a window told it has lost the keyboard and the mouse, as SDL tells it, wherever the keyboard is */
static void focus_lost(const host_window_t *w)
{
    SDL_Event e = {0};
    e.type = SDL_WINDOWEVENT, e.window.windowID = w->id, e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    CHECK(SDL_PushEvent(&e) == 1);
}

/* the keyboard taken from the windows, read so, and given back: SDL's dummy driver gives it to a window shown and takes it from one hidden */
static void keyboard_away(void)
{
    SDL_Window *had = SDL_GetKeyboardFocus();
    CHECK(had != NULL);
    if (!had) return;
    SDL_HideWindow(had);
    dgx_host_delay(0);
    CHECK(SDL_GetKeyboardFocus() == NULL);
    SDL_ShowWindow(had);
}

/* a script read from the text, in place of the steps before, its times from now on: NULL, or what is wrong and its line */
static const char *script(const char *text, int *line)
{
    steps_n = steps_done = 0;
    FILE *f              = fmemopen((void *)text, strlen(text), "r");
    CHECK(f != NULL);
    const char *wrong = f ? script_read(f, line) : "no file";
    if (f) fclose(f);
    begun_us = clock_us();
    return wrong;
}

/* the line of a script that is wrong */
static int wrong_at(const char *text)
{
    int line = 0;
    return script(text, &line) ? line : 0;
}

/* the one finger a screen has now, or -1, -1 */
static void finger(dgx_screen_t *scr, int *x, int *y)
{
    dgx_host_finger_t f[2] = {{-1, -1, 9}, {-1, -1, 9}};
    const int         n    = dgx_host_screen_fingers(scr, f, 2);
    CHECK(n == 0 || (n == 1 && f[0].id == 0));
    *x = n ? f[0].x : -1, *y = n ? f[0].y : -1;
}

void app_main(void)
{
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    dgx_screen_t *host = dgx_host_screen_init(W, H, 16, NULL);
    CHECK(host != NULL);
    if (!host) exit(EXIT_FAILURE);
    host_window_t *one = window_of(host);
    CHECK(one->number == 1 && !strcmp(one->title, "DGX") && !quad(one) && one->protocols.bus_type == DGX_BUS_SPI);
    CHECK(one->protocols.buffer_len == 4736); /* as the SPI bus of the CYD has it at 40 MHz */
    CHECK(one->transfer_us == 0.75 && one->piece_us == 25.0 && one->turn == 0);

    /* a second display, on QSPI at 20 MHz, its buffer limited, turned on the board, its transfers costing nothing above the wire */
    const dgx_host_screen_options_t qspi = {
        .bus_hz = 20000000, .bus_lines = 4, .buffer_max = 1000, .transfer_us = -1, .piece_us = -1, .turn = 90};
    dgx_screen_t *bar = dgx_host_screen_init(32, 16, 16, &qspi);
    CHECK(bar != NULL);
    if (!bar) exit(EXIT_FAILURE);
    host_window_t *two = window_of(bar);
    CHECK(two != one && two->number == 2 && !strcmp(two->title, "DGX 2") && one->next == two && !two->next);
    CHECK(quad(two) && two->protocols.bus_type == DGX_BUS_QSPI && two->protocols.buffer_len == 992 && two->transfer_us == 0 &&
          two->piece_us == 0 && two->turn == 90);
    CHECK(dgx_host_screen_init(0, 10, 16, NULL) == NULL);
    CHECK(dgx_host_screen_init(8, 8, 16, &(dgx_host_screen_options_t){.bus_lines = 2}) == NULL);
    CHECK(dgx_host_screen_init(8, 8, 16, &(dgx_host_screen_options_t){.turn = 45}) == NULL);
    CHECK(windows == one && one->next == two && !two->next);

    /* SPI, the whole screen filled: the window and its memory write by the registers, the pixels in two pieces of the buffer by DMA */
    dgx_fill_rectangle(host, 0, 0, W, H, dgx_rgb_to_16(1, 2, 3));
    const double fill_wire = (11 + W * H * 2) * 8 / 40.0;
    CHECK(one->transfers == 5 && one->pieces == 2 && one->bytes == W * H * 2 + 11 && two->transfers + two->pieces == 0);
    CHECK(near(one->wire_us, fill_wire) && near(one->us, fill_wire + 5 * 0.75 + 2 * 25.0));
    dgx_host_delay(0);
    /* both made before the first wait: put side by side, each glass the same number of times as large, the second turned */
    int one_w, one_h, two_w, two_h;
    SDL_GetWindowSize(one->window, &one_w, &one_h);
    SDL_GetWindowSize(two->window, &two_w, &two_h);
    CHECK(one->placed && two->placed && one_w > W && one_w * H == one_h * W && two_w * W == one_w * 16 && two_h * W == one_w * 32);

    /*
     * The mouse: pressed on a window it is a finger, at the pixel of the glass
     * under it; moved with the button held, it follows, pressed to the edge
     * outside; let go, or the window left, it is gone. A turned window gives
     * the pixel of the glass upright.
     */
    {
        const int zoom = one_w / W;
        int       x, y;
        finger(host, &x, &y);
        CHECK(x == -1);
        mouse(one, SDL_MOUSEBUTTONDOWN, 10 * zoom + zoom / 2, 20 * zoom + zoom / 2);
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == 10 && y == 20);
        finger(bar, &x, &y);
        CHECK(x == -1);
        mouse(one, SDL_MOUSEMOTION, -50, 5000);
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == 0 && y == H - 1);
        mouse(one, SDL_MOUSEBUTTONUP, 0, 0);
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == -1);
        mouse(one, SDL_MOUSEMOTION, 10 * zoom, 10 * zoom); /* no button: no finger */
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == -1);
        /* the second window shows its 32x16 glass turned by 90 degrees: what is seen at 3, 5 is the pixel 5, 15 - 3 */
        mouse(two, SDL_MOUSEBUTTONDOWN, 3 * zoom + zoom / 2, 5 * zoom + zoom / 2);
        dgx_host_delay(0);
        finger(bar, &x, &y);
        CHECK(x == 5 && y == 12);
        SDL_Event lost = {0};
        lost.type = SDL_WINDOWEVENT, lost.window.windowID = two->id, lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
        CHECK(SDL_PushEvent(&lost) == 1);
        dgx_host_delay(0);
        finger(bar, &x, &y);
        CHECK(x == -1);
        /* a screen that is not a window has none */
        dgx_screen_t     *v = dgx_vscreen_init(8, 8, 16, DgxScreenRGB);
        dgx_host_finger_t f;
        CHECK(v && dgx_host_screen_fingers(v, &f, 1) == 0 && dgx_host_screen_fingers(NULL, &f, 1) == 0 && dgx_host_screen_fingers(host, &f, 0) == 0);
        dgx_screen_destroy(&v);
    }

    /*
     * The keys: the program's, whatever window has the keyboard, held until
     * they go up, held on as the computer repeats them, written in ascending
     * order; let go of when the keyboard leaves the windows.
     */
    {
        uint16_t k[8];
        CHECK(held(k) == 0);
        key(one, SDL_KEYDOWN, SDL_SCANCODE_LEFT, 0);
        key(one, SDL_KEYDOWN, SDL_SCANCODE_A, 0);
        key(one, SDL_KEYDOWN, SDL_SCANCODE_LEFT, 1);
        key(two, SDL_KEYDOWN, SDL_SCANCODE_RSHIFT, 0);
        dgx_host_delay(0);
        CHECK(held(k) == 3 && k[0] == SDL_SCANCODE_A && k[1] == SDL_SCANCODE_LEFT && k[2] == SDL_SCANCODE_RSHIFT && SDL_SCANCODE_RSHIFT == 229);
        CHECK(dgx_host_keys(k, 1) == 1 && k[0] == SDL_SCANCODE_A && dgx_host_keys(k, 0) == 0 && dgx_host_keys(NULL, 8) == 0);
        key(two, SDL_KEYUP, SDL_SCANCODE_A, 0); /* up on the other window */
        dgx_host_delay(0);
        CHECK(held(k) == 2 && k[0] == SDL_SCANCODE_LEFT && k[1] == SDL_SCANCODE_RSHIFT);
        CHECK(SDL_GetKeyboardFocus() == two->window); /* shown last */
        focus_lost(one);                               /* the keyboard on another window of the program: the keys stay */
        dgx_host_delay(0);
        CHECK(held(k) == 2);
        keyboard_away();
        CHECK(held(k) == 0);
    }

    /*
     * A script: its steps played when their time comes. Its keys are held
     * with those of the computer, and the keyboard leaving the windows lets go
     * of the computer's alone. Its mouse lies on the pixel of the glass named,
     * through the turn of a window, until the script lets it go, whatever the
     * window loses. A script that is wrong tells its line.
     */
    {
        int      line = 0, x, y;
        uint16_t k[8];
        CHECK(!script("# keys\n\n  0 key down Left Shift\r\n0 key down space\n1000000 key up Left Shift\n", &line) && steps_n == 3);
        CHECK(steps[1].key == SDL_SCANCODE_SPACE && steps[1].line == 4 && steps[2].ms == 1000000);
        key(one, SDL_KEYDOWN, SDL_SCANCODE_UP, 0);
        key(one, SDL_KEYDOWN, SDL_SCANCODE_SPACE, 0);
        dgx_host_delay(0);
        CHECK(held(k) == 3 && k[0] == SDL_SCANCODE_SPACE && k[1] == SDL_SCANCODE_UP && k[2] == SDL_SCANCODE_LSHIFT && steps_done == 2);
        keyboard_away();
        CHECK(held(k) == 2 && k[0] == SDL_SCANCODE_SPACE && k[1] == SDL_SCANCODE_LSHIFT);
        CHECK(!script("0 key up left shift\n0 key up Space\n50 key down Z\n", &line));
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000}, NULL);
        dgx_host_delay(0);
        CHECK(held(k) == 0); /* Z not yet */
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000}, NULL);
        dgx_host_delay(0);
        CHECK(held(k) == 1 && k[0] == SDL_SCANCODE_Z);
        CHECK(!script("0 key up z\n", &line));
        dgx_host_delay(0);
        CHECK(held(k) == 0);

        CHECK(!script("0 mouse down 10 20\n1000000 mouse up\n", &line));
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == 10 && y == 20);
        focus_lost(one);
        mouse(one, SDL_MOUSEBUTTONUP, 0, 0);
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == 10 && y == 20);
        CHECK(!script("0 mouse down 0 0\n0 mouse move -5 900\n1000000 mouse up\n", &line));
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == 0 && y == H - 1);
        CHECK(!script("0 mouse down 3 3\n0 mouse up\n", &line));
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == -1);
        /* the second window shows its 32x16 glass turned by 90 degrees: the script names the pixel of the glass upright */
        CHECK(!script("0 window 2 mouse down 5 12\n0 window 2 mouse move 31 15\n1000000 window 2 mouse up\n", &line));
        dgx_host_delay(0);
        finger(bar, &x, &y);
        CHECK(x == 31 && y == 15);
        finger(host, &x, &y);
        CHECK(x == -1);
        CHECK(!script("0 window 2 mouse down 0 0\n0 window 2 mouse up\n", &line));
        dgx_host_delay(0);
        finger(bar, &x, &y);
        CHECK(x == -1);
        /* the mouse of the computer and that of the script on one window: the finger lies where the last put it, until both let go */
        const int zoom = one_w / W;
        mouse(one, SDL_MOUSEBUTTONDOWN, 10 * zoom + zoom / 2, 20 * zoom + zoom / 2);
        dgx_host_delay(0);
        CHECK(!script("0 mouse down 1 2\n0 mouse up\n", &line));
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == 1 && y == 2);
        mouse(one, SDL_MOUSEBUTTONUP, 0, 0);
        dgx_host_delay(0);
        finger(host, &x, &y);
        CHECK(x == -1);

        CHECK(wrong_at("abc\n") == 1 && wrong_at("# c\n\n10 key down NoSuchKey\n") == 3 && wrong_at("10 key down\n") == 1);
        CHECK(wrong_at("10 key sideways A\n") == 1 && wrong_at("20 key down A\n10 key up A\n") == 2 && wrong_at("10 window 2 key down A\n") == 1);
        CHECK(wrong_at("10 window 0 mouse down 1 1\n") == 1 && wrong_at("10 mouse down 5\n") == 1 && wrong_at("10 mouse down 1 2 3\n") == 1);
        CHECK(wrong_at("10 mouse up\n") == 1 && wrong_at("10 mouse move 1 1\n") == 1 && wrong_at("1 mouse down 1 2\n2 mouse down 3 4\n") == 2);
        CHECK(wrong_at("1 mouse down 1 2\n2 window 2 mouse move 3 4\n") == 2 && wrong_at("1 mouse down 40000 2\n") == 1);
        CHECK(wrong_at("-1 key down A\n") == 1 && wrong_at("5 jump\n") == 1 && wrong_at("5 window 2\n") == 1);
        char long_line[300];
        memset(long_line, 'x', sizeof(long_line) - 2);
        long_line[sizeof(long_line) - 2] = '\n', long_line[sizeof(long_line) - 1] = 0;
        CHECK(wrong_at(long_line) == 1);
        steps_n = steps_done = 0;
    }

    dgx_host_delay(0); /* a wait with nothing sent is no pass */
    CHECK(one->passes == 1 && near(one->most_us, fill_wire + 5 * 0.75 + 2 * 25.0) && one->us == 0 && two->passes == 0);

    /* SPI, a point of 3x3: both bounds of the window, the memory write and 18 bytes, six transfers by the registers */
    dgx_fill_rectangle(host, 5, 6, 3, 3, dgx_rgb_to_16(255, 255, 255));
    CHECK(one->transfers == 6 && one->pieces == 0 && near(one->wire_us, 29 * 8 / 40.0) && near(one->us, one->wire_us + 6 * 0.75));
    dgx_host_delay(0);

    /*
     * QSPI, the whole glass of 1024 bytes: the bounds with their commands, 64
     * clocks each; the memory write with a buffer of 992 bytes by DMA, its
     * opcode and address on one line and the pixels on four; the 32 bytes
     * left by the registers. The other glass has not changed.
     */
    const uint64_t first = glass_sum(one);
    dgx_fill_rectangle(bar, 0, 0, 32, 16, dgx_rgb_to_16(255, 0, 0));
    CHECK(two->glass[0] == 0xFFFF0000u && two->glass[32 * 16 - 1] == 0xFFFF0000u && glass_sum(one) == first);
    CHECK(two->transfers == 3 && two->pieces == 1 && near(two->wire_us, (64 + 64 + (32 + 992 * 2) + (32 + 32 * 2)) / 20.0) &&
          near(two->us, two->wire_us));
    dgx_host_delay(0);
    CHECK(two->passes == 1 && one->passes == 2);
    /* QSPI, a point of 3x3: 64 + 64 + 32 + 36 clocks, not the bytes over four lines */
    dgx_fill_rectangle(bar, 5, 6, 3, 3, dgx_rgb_to_16(0, 255, 0));
    CHECK(two->transfers == 3 && two->pieces == 0 && near(two->wire_us, 196 / 20.0));
    dgx_host_delay(0);

    /* the glass as the board is held: turned clockwise, its top-left pixel goes to the top-right, and so on */
    check_turn(two, 0, 0, 0, 31, 0, 0, 15);
    check_turn(two, 90, 15, 0, 15, 31, 0, 0);
    check_turn(two, 180, 31, 15, 0, 15, 31, 0);
    check_turn(two, 270, 0, 31, 0, 0, 15, 31);

    check_every_way(host, 16);
    check_every_way(bar, 16);

    /* 18 bits: another screen of the same panel, as a program cannot make it, with three bytes a pixel */
    dgx_screen_t *deep = dgx_panel_init(&one->protocols, &one->description, GPIO_NUM_NC, GPIO_NUM_NC, 18, DgxScreenRGB);
    CHECK(deep != NULL && one->colmod == 0x66);
    if (deep) check_every_way(deep, 18);
    CHECK(dgx_panel_init(&one->protocols, &one->description, GPIO_NUM_NC, GPIO_NUM_NC, 12, DgxScreenRGB) == NULL);

    /* a program that does not wait: a window made later is shown by its pixels once no window has been made for a while */
    dgx_screen_t *late = dgx_host_screen_init(8, 8, 16, NULL);
    CHECK(late != NULL);
    if (!late) exit(EXIT_FAILURE);
    host_window_t *three = window_of(late);
    dgx_fill_rectangle(late, 0, 0, 8, 8, dgx_rgb_to_16(0, 0, 255));
    CHECK(!three->placed);
    nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 60 * 1000 * 1000}, NULL);
    dgx_fill_rectangle(late, 0, 0, 8, 8, dgx_rgb_to_16(0, 255, 255));
    CHECK(three->placed);
    dgx_screen_destroy(&late);
    three->protocols.dispose(&three->protocols);
    CHECK(windows == one && one->next == two && !two->next);

    /* a display on no bus the window stands for: drawn, nothing counted, and its title says nothing of a bus */
    dgx_screen_t *dsi = dgx_host_screen_init(8, 8, 16, &(dgx_host_screen_options_t){.no_bus = true});
    CHECK(dsi != NULL);
    if (!dsi) exit(EXIT_FAILURE);
    host_window_t *four = window_of(dsi);
    dgx_fill_rectangle(dsi, 0, 0, 8, 8, dgx_rgb_to_16(0, 0, 255));
    CHECK(four->glass[0] == 0xFF0000FFu && four->transfers + four->pieces + four->bytes == 0 && four->us == 0);
    dgx_host_delay(0);
    tell_title(four);
    tell_title(one);
    CHECK(!strstr(SDL_GetWindowTitle(four->window), "bus") && strstr(SDL_GetWindowTitle(one->window), "bus") && four->passes == 0);
    dgx_screen_destroy(&dsi);
    four->protocols.dispose(&four->protocols);
    CHECK(windows == one && one->next == two && !two->next);

    /*
     * A display on MIPI-DSI is read, and takes 24 bits: what is drawn on the screen of such a window comes back from
     * dgx_get_pixel() as it was given, at 16, 18 and 24 bits, the screen turned all eight ways. A window of a panel
     * on SPI is not read and takes no 24 bits.
     */
    {
        CHECK(dgx_host_screen_init(8, 8, 24, NULL) == NULL);
        dgx_screen_t *spi = dgx_host_screen_init(8, 8, 16, NULL);
        CHECK(spi && !dgx_screen_is_readable(spi));
        if (spi) {
            host_window_t *w = window_of(spi);
            dgx_screen_destroy(&spi);
            w->protocols.dispose(&w->protocols);
        }
        static const uint8_t depths[3] = {16, 18, 24};
        uint32_t             seed      = 12345;
        for (int d = 0; d < 3; ++d) {
            dgx_screen_t *glass = dgx_host_screen_init(12, 7, depths[d], &(dgx_host_screen_options_t){.no_bus = true});
            CHECK(glass && dgx_screen_is_readable(glass));
            if (!glass) continue;
            host_window_t *w = window_of(glass);
            for (int o = 0; o < 8; ++o) {
                dgx_panel_orientation(glass, (o & 1) ? DgxScreenRightLeft : DgxScreenLeftRight, (o & 2) ? DgxScreenBottomTop : DgxScreenTopBottom,
                                      (o & 4) != 0);
                CHECK(glass->width == ((o & 4) ? 7 : 12) && glass->height == ((o & 4) ? 12 : 7));
                int wrong = 0;
                for (int y = 0; y < glass->height; ++y)
                    for (int x = 0; x < glass->width; ++x) {
                        seed = seed * 1664525u + 1013904223u;
                        const uint32_t c = depths[d] == 16 ? seed >> 16 : depths[d] == 18 ? (seed >> 8) & 0xFCFCFCu : seed >> 8;
                        dgx_set_pixel(glass, x, y, c);
                        wrong += dgx_get_pixel(glass, x, y) != c;
                    }
                /* a rectangle through the window of the controller, read back pixel by pixel */
                const uint32_t c = depths[d] == 16 ? 0xF81Fu : depths[d] == 18 ? 0x40FC80u : 0x123456u;
                dgx_fill_rectangle(glass, 1, 2, 4, 3, c);
                for (int y = 2; y < 5; ++y)
                    for (int x = 1; x < 5; ++x) wrong += dgx_get_pixel(glass, x, y) != c;
                CHECK(wrong == 0);
            }
            /* 24 bits reach the glass whole */
            dgx_panel_orientation(glass, DgxScreenLeftRight, DgxScreenTopBottom, false);
            if (depths[d] == 24) {
                dgx_set_pixel(glass, 0, 0, 0x123457u);
                CHECK((w->glass[0] & 0xFFFFFFu) == 0x123457u);
            }
            dgx_screen_destroy(&glass);
            w->protocols.dispose(&w->protocols);
        }
        CHECK(windows == one && one->next == two && !two->next);
    }

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

    /* what the program is told of the memory left: the limits, as heap_caps and esp_system have it on the board */
    CHECK(heap_caps_get_free_size(MALLOC_CAP_8BIT) == 2000 && esp_get_free_heap_size() == 2000);
    CHECK(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) == 2000);
    CHECK(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) == (heap.most < heap.limit ? heap.limit - heap.most : 0));
    CHECK(esp_get_minimum_free_heap_size() == heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    heap.block_limit = 500;
    CHECK(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) == 500);
    heap.block_limit = 0;
    heap.limit       = 0;
    CHECK(heap_caps_get_free_size(MALLOC_CAP_8BIT) == ((size_t)1 << 30) - heap.now);

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
