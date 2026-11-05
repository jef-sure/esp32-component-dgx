/*
 * host_screen.c
 *
 *  The displays of dgx_host_screen.h: a panel of the MIPI display command
 *  set whose memory is the glass a window of SDL2 shows, behind a bus that
 *  puts every byte into it at once and counts what it would cost on the wire
 *  of the board. The screen on it is made by dgx_panel_init(), as the screens
 *  of the ILI9341 and the ST7789 are. Each window has its panel and its bus;
 *  the program has the clock, the memory and the waits once, as a chip has.
 *
 *  The waits of the program, vTaskDelay() and dgx_delay(), are waits here
 *  too, and the windows are shown and their events read in them: a program
 *  for a board waits in every pass of its loop. A pass is what the program
 *  sends from one wait to the next, and the wire time of its bytes is what
 *  the title of a window tells.
 *
 *  Every malloc, calloc, realloc and free of the program and of DGX comes
 *  through here (-Wl,--wrap of DGX_HOST_WINDOW_LDLIBS) and is counted; the
 *  window's own memory and SDL's are not.
 *
 *  Variables of the environment, for runs with nobody at the computer and
 *  for a board with less memory:
 *    DGX_HOST_QUIT_MS    the program ends so many ms after the first window was made;
 *    DGX_HOST_SHOT       the glass written to that file when the program ends (a PPM), that
 *                        of a second window to the same name with -2 before its extension;
 *    DGX_HOST_HEAP_MAX   bytes the program and DGX may hold at once: an allocation over it gets NULL;
 *    DGX_HOST_BLOCK_MAX  the largest block they may get at once.
 *  With SDL_VIDEODRIVER=dummy no window opens.
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include "dgx_host_screen.h"

#include <SDL2/SDL.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#include "drivers/dgx_lcd_init.h"
#include "drivers/dgx_panel.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char TAG[] = "DGX HOST";

#define HOST_CASET  0x2Au
#define HOST_RASET  0x2Bu
#define HOST_RAMWR  0x2Cu
#define HOST_MADCTL 0x36u
#define HOST_COLMOD 0x3Au
#define HOST_RAMWRC 0x3Cu

#define HOST_MY  0x80u
#define HOST_MX  0x40u
#define HOST_MV  0x20u
#define HOST_BGR 0x08u

/* the glass shown no more often than this; while pixels go with no wait, at least so often */
#define HOST_SHOW_US      16000
#define HOST_SHOW_BUSY_US 50000
/* the title told anew so often */
#define HOST_TITLE_US 500000

/*
 * The buffer of the bus as dgx_spi_buffer_len() makes it on the board: a
 * half of it takes about 475 us on the wire, it is 4000 bytes at least and
 * 32000 at most, a multiple of 16. The pieces the screen sends are as long
 * as on the board then.
 */
#define HOST_HALF_US     475u
#define HOST_MIN_BUFFER  4000u
#define HOST_MAX_TRANSFER 32000u

/* ---- the clock ---- */

static int64_t started_us;

static int64_t clock_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

__attribute__((constructor)) static void clock_start(void)
{
    started_us = clock_us();
}

/* the timer of the board: microseconds since the program began, as since the chip did */
int64_t esp_timer_get_time(void)
{
    return clock_us() - started_us;
}

/* ---- the memory the program and DGX hold ---- */

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
void  __real_free(void *p);

static struct {
    size_t now;         /* held */
    size_t most;        /* held at most at once */
    size_t block;       /* the largest block asked for */
    size_t limit;       /* DGX_HOST_HEAP_MAX, 0 for none */
    size_t block_limit; /* DGX_HOST_BLOCK_MAX, 0 for none */
    unsigned refused;   /* allocations that got NULL for a limit */
} heap;

/* whether size more fits, with freed of what is held let go of at the same time */
static bool heap_lets(size_t size, size_t freed)
{
    if (heap.block_limit && size > heap.block_limit) return false;
    if (heap.limit && heap.now - freed + size > heap.limit) return false;
    return true;
}

static void *heap_refuse(size_t size)
{
    ++heap.refused;
    fprintf(stderr, "W %s: no memory for %zu bytes: %zu held, DGX_HOST_HEAP_MAX %zu, DGX_HOST_BLOCK_MAX %zu\n", TAG, size, heap.now,
            heap.limit, heap.block_limit);
    return NULL;
}

static void heap_took(void *p, size_t size)
{
    if (!p) return;
    heap.now += malloc_usable_size(p);
    if (heap.now > heap.most) heap.most = heap.now;
    if (size > heap.block) heap.block = size;
}

/* memory let go of; a block the C library gave by itself, as strdup() does, was never counted */
static void heap_gave(size_t size)
{
    heap.now -= size < heap.now ? size : heap.now;
}

void *__wrap_malloc(size_t size)
{
    if (!heap_lets(size, 0)) return heap_refuse(size);
    void *p = __real_malloc(size);
    heap_took(p, size);
    return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) return NULL;
    if (!heap_lets(n * size, 0)) return heap_refuse(n * size);
    void *p = __real_calloc(n, size);
    heap_took(p, n * size);
    return p;
}

void *__wrap_realloc(void *p, size_t size)
{
    const size_t had = p ? malloc_usable_size(p) : 0;
    if (!heap_lets(size, had)) return heap_refuse(size);
    void *q = __real_realloc(p, size);
    /* realloc(p, 0) lets p go and gives NULL; a NULL for a size is a failure, and p stays */
    if (q || (p && !size)) heap_gave(had);
    heap_took(q, size);
    return q;
}

void __wrap_free(void *p)
{
    if (p) heap_gave(malloc_usable_size(p));
    __real_free(p);
}

/* ---- a window: the panel of a display behind its bus, and what the bus has sent ---- */

typedef struct host_window_t {
    dgx_bus_protocols_t   protocols; /* first: the bus of a screen leads to its window */
    struct host_window_t *next;
    int                   number;      /* 1 for the first window of the program */
    dgx_panel_t           description; /* what dgx_panel_init() makes the screen from */
    /* the controller, as far as the window, MADCTL and COLMOD go; its memory is the glass */
    int       width, height;    /* the glass upright, and the memory as large */
    uint32_t *glass;            /* ARGB8888, as the window takes it */
    uint8_t   command;          /* the last command */
    uint8_t   params[4];
    int       params_n;
    uint16_t  sc, ec, sp, ep;   /* the window of the controller: its columns and pages */
    uint16_t  column, page;     /* the address of the next pixel */
    uint8_t   madctl, colmod;
    uint8_t   part[3];          /* the bytes of a pixel so far */
    int       part_n;
    bool      changed;          /* since the glass was shown */
    /* the bus of the board and what it has sent */
    uint32_t  bus_hz;
    uint8_t   bus_lines;
    uint64_t  bytes, transfers; /* of the pass going on */
    uint64_t  passes;           /* the passes that sent something */
    double    sum_us, most_us;
    uint64_t  most_bytes, most_transfers;
    uint32_t  recent;           /* and those since the title was told */
    double    recent_sum_us, recent_most_us;
    /* the window on the computer */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    uint32_t      id;
    bool          placed;       /* sized and put beside the others */
    char          title[64];
    int64_t       shown_us;
} host_window_t;

/*
 * What a program has once, as a chip has: its windows, in the order they
 * were made, and the waits that show them; the clock and the memory above.
 */
static host_window_t *windows;
static int            made;     /* windows made so far */
static int64_t        pumped_us, titled_us, quit_us;
static bool           waited;   /* the program has come to its first wait: the windows are shown from then on */
static const char    *shot;

static int pixel_bytes(const host_window_t *w)
{
    return (w->colmod & 0x07u) == 0x06u ? 3 : 2;
}

static void panel_pixel(host_window_t *w, const uint8_t *b)
{
    uint32_t r, g, bl;
    if (pixel_bytes(w) == 3) {
        /* 18 bits: a byte a color, its six bits at the top */
        r  = b[0] & 0xFCu;
        g  = b[1] & 0xFCu;
        bl = b[2] & 0xFCu;
        r |= r >> 6, g |= g >> 6, bl |= bl >> 6;
    } else {
        const uint32_t v = (uint32_t)b[0] << 8 | b[1];
        r                = v >> 11 & 31u;
        g                = v >> 5 & 63u;
        bl               = v & 31u;
        r = r << 3 | r >> 2, g = g << 2 | g >> 4, bl = bl << 3 | bl >> 2;
    }
    if (w->madctl & HOST_BGR) {
        const uint32_t t = r;
        r = bl, bl = t;
    }
    /* the column address is that of the rows of memory when the axes are exchanged; MY turns the rows, MX the columns */
    const bool swap = w->madctl & HOST_MV;
    int        x    = swap ? w->page : w->column;
    int        y    = swap ? w->column : w->page;
    if (w->madctl & HOST_MX) x = w->width - 1 - x;
    if (w->madctl & HOST_MY) y = w->height - 1 - y;
    if (x >= 0 && x < w->width && y >= 0 && y < w->height) {
        w->glass[(size_t)y * w->width + x] = 0xFF000000u | r << 16 | g << 8 | bl;
        w->changed                         = true;
    }
    if (++w->column > w->ec) {
        w->column = w->sc;
        if (++w->page > w->ep) w->page = w->sp;
    }
}

static void panel_command(host_window_t *w, uint8_t command)
{
    w->command  = command;
    w->params_n = 0;
    w->part_n   = 0;
    if (command == HOST_RAMWR) w->column = w->sc, w->page = w->sp;
}

static void panel_data(host_window_t *w, const uint8_t *data, uint32_t bytes)
{
    if (w->command == HOST_RAMWR || w->command == HOST_RAMWRC) {
        const int n = pixel_bytes(w);
        for (uint32_t i = 0; i < bytes; ++i) {
            w->part[w->part_n++] = data[i];
            if (w->part_n == n) panel_pixel(w, w->part), w->part_n = 0;
        }
        return;
    }
    for (uint32_t i = 0; i < bytes; ++i) {
        if (w->params_n < (int)sizeof(w->params)) w->params[w->params_n] = data[i];
        const int n = ++w->params_n;
        if (w->command == HOST_CASET && n == 4) {
            w->sc = (uint16_t)(w->params[0] << 8 | w->params[1]);
            w->ec = (uint16_t)(w->params[2] << 8 | w->params[3]);
        } else if (w->command == HOST_RASET && n == 4) {
            w->sp = (uint16_t)(w->params[0] << 8 | w->params[1]);
            w->ep = (uint16_t)(w->params[2] << 8 | w->params[3]);
        } else if (w->command == HOST_MADCTL && n == 1) {
            w->madctl = w->params[0];
        } else if (w->command == HOST_COLMOD && n == 1) {
            w->colmod = w->params[0];
        }
    }
}

/* ---- passes and the wire ---- */

static double wire_us(const host_window_t *w, uint64_t bytes)
{
    return (double)bytes * 8.0 * 1e6 / ((double)w->bus_hz * w->bus_lines);
}

static void pass_end(host_window_t *w)
{
    if (!w->bytes) return;
    const double us = wire_us(w, w->bytes);
    ++w->passes;
    w->sum_us += us;
    if (us > w->most_us) w->most_us = us, w->most_bytes = w->bytes, w->most_transfers = w->transfers;
    ++w->recent;
    w->recent_sum_us += us;
    if (us > w->recent_most_us) w->recent_most_us = us;
    w->bytes = w->transfers = 0;
}

static void stats_clear(host_window_t *w)
{
    w->bytes = w->transfers = w->passes = w->most_bytes = w->most_transfers = 0;
    w->sum_us = w->most_us = w->recent_sum_us = w->recent_most_us = 0;
    w->recent                                                     = 0;
}

/* ---- the windows on the computer ---- */

static void write_shot(const host_window_t *w)
{
    /* the first window to the file named, the others beside it: shot-2.ppm */
    char        path[4096];
    const char *dot   = strrchr(shot, '.');
    const char *slash = strrchr(shot, '/');
    if (w->number == 1) snprintf(path, sizeof(path), "%s", shot);
    else if (dot && (!slash || dot > slash)) snprintf(path, sizeof(path), "%.*s-%d%s", (int)(dot - shot), shot, w->number, dot);
    else snprintf(path, sizeof(path), "%s-%d", shot, w->number);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "no picture to %s", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w->width, w->height);
    for (size_t i = 0; i < (size_t)w->width * w->height; ++i) {
        const uint8_t rgb[3] = {(uint8_t)(w->glass[i] >> 16), (uint8_t)(w->glass[i] >> 8), (uint8_t)w->glass[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void tell_title(host_window_t *w)
{
    char text[256];
    int  n = snprintf(text, sizeof(text), "%s %dx%d", w->title, w->width, w->height);
    if (w->recent)
        n += snprintf(text + n, sizeof(text) - n, "  |  bus %.1f ms a pass, %.1f at most", w->recent_sum_us / w->recent / 1000.0,
                      w->recent_most_us / 1000.0);
    else
        n += snprintf(text + n, sizeof(text) - n, "  |  bus idle");
    snprintf(text + n, sizeof(text) - n, "  |  heap %.1f KB, %.1f at most", heap.now / 1024.0, heap.most / 1024.0);
    SDL_SetWindowTitle(w->window, text);
    w->recent         = 0;
    w->recent_sum_us  = 0;
    w->recent_most_us = 0;
}

/* what the program has done, when it ends */
static void host_end(void)
{
    for (host_window_t *w = windows; w; w = w->next) {
        pass_end(w);
        if (shot && *shot) write_shot(w);
        fprintf(stderr,
                "I %s: window %d, %dx%d: the bus at %.1f MHz over %u line%s, passes that sent something: %llu; on the wire %.2f ms a pass "
                "on average, %.2f at most (%llu bytes in %llu transfers)\n",
                TAG, w->number, w->width, w->height, w->bus_hz / 1e6, w->bus_lines, w->bus_lines == 1 ? "" : "s",
                (unsigned long long)w->passes, w->passes ? w->sum_us / w->passes / 1000.0 : 0.0, w->most_us / 1000.0,
                (unsigned long long)w->most_bytes, (unsigned long long)w->most_transfers);
    }
    fprintf(stderr, "I %s: memory: %zu bytes held now, %zu at most at once, the largest block %zu", TAG, heap.now, heap.most, heap.block);
    if (heap.refused) fprintf(stderr, "; %u allocations refused", heap.refused);
    fputc('\n', stderr);
    SDL_Quit();
}

/*
 * The windows made since the last wait, side by side in the middle of the
 * screen of the computer, the glass of each the same whole number of times
 * as large, as large as they all fit. Those made earlier stay where they are.
 */
static void place(void)
{
    const int gap   = 16;
    int       total = 0, high = 0, n = 0;
    for (host_window_t *w = windows; w; w = w->next) {
        if (w->placed) continue;
        total += w->width;
        if (w->height > high) high = w->height;
        ++n;
    }
    if (!n) return;
    SDL_Rect usable = {0, 0, 1280, 800};
    SDL_GetDisplayUsableBounds(0, &usable);
    int zoom = 1;
    while (total * (zoom + 1) + gap * (n - 1) <= usable.w * 9 / 10 && high * (zoom + 1) <= usable.h * 9 / 10) ++zoom;
    int x = usable.x + (usable.w - total * zoom - gap * (n - 1)) / 2;
    for (host_window_t *w = windows; w; w = w->next) {
        if (w->placed) continue;
        SDL_SetWindowSize(w->window, w->width * zoom, w->height * zoom);
        SDL_SetWindowPosition(w->window, x, usable.y + (usable.h - w->height * zoom) / 2);
        SDL_ShowWindow(w->window);
        ESP_LOGI(TAG, "window %d: the glass %d times as large, %dx%d", w->number, zoom, w->width * zoom, w->height * zoom);
        x += w->width * zoom + gap;
        w->placed  = true;
        w->changed = true;
    }
}

/* the events of the windows read: a closed window ends the program, as there is one board */
static void pump(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) exit(EXIT_SUCCESS);
        if (event.type != SDL_WINDOWEVENT) continue;
        if (event.window.event == SDL_WINDOWEVENT_CLOSE) exit(EXIT_SUCCESS);
        for (host_window_t *w = windows; w; w = w->next) /* uncovered, stretched: drawn again */
            if (w->id == event.window.windowID) w->changed = true;
    }
}

/* every window shown, the ones whose glass has changed, when it is time */
static void show(int64_t every_us)
{
    if (!windows) return;
    place();
    pump();
    const int64_t now = clock_us();
    for (host_window_t *w = windows; w; w = w->next) {
        if (!w->changed || now - w->shown_us < every_us) continue;
        SDL_UpdateTexture(w->texture, NULL, w->glass, w->width * (int)sizeof(*w->glass));
        SDL_RenderClear(w->renderer);
        SDL_RenderCopy(w->renderer, w->texture, NULL, NULL);
        SDL_RenderPresent(w->renderer);
        w->shown_us = now;
        w->changed  = false;
    }
    if (now - titled_us >= HOST_TITLE_US) {
        for (host_window_t *w = windows; w; w = w->next) tell_title(w);
        titled_us = now;
    }
    pumped_us = now;
    if (quit_us && now >= quit_us) exit(EXIT_SUCCESS);
}

/* the wait of the program: vTaskDelay() and dgx_delay() of host/stubs with DGX_HOST_WINDOW; it ends a pass on every bus */
void dgx_host_delay(uint32_t ms)
{
    waited = true;
    for (host_window_t *w = windows; w; w = w->next) pass_end(w);
    const int64_t until = clock_us() + (int64_t)ms * 1000;
    for (;;) {
        show(HOST_SHOW_US);
        const int64_t left = until - clock_us();
        if (left <= 0) break;
        const int64_t   step = left < 4000 ? left : 4000;
        struct timespec t    = {.tv_sec = 0, .tv_nsec = (long)step * 1000};
        nanosleep(&t, NULL);
    }
}

/* ---- the bus ---- */

static void sent(host_window_t *w, uint32_t bytes)
{
    w->bytes += bytes;
    ++w->transfers;
    if (waited && clock_us() - pumped_us >= HOST_SHOW_BUSY_US) show(HOST_SHOW_BUSY_US);
}

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t command)
{
    host_window_t *w = (host_window_t *)bus;
    panel_command(w, command);
    sent(w, 1);
}

static void bus_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    host_window_t *w = (host_window_t *)bus;
    for (uint32_t i = 0; i < len / 8; ++i) panel_command(w, data[i]);
    sent(w, len / 8);
}

static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    host_window_t *w = (host_window_t *)bus;
    panel_data(w, data, len / 8);
    sent(w, len / 8);
}

static uint32_t bus_read(struct _dgx_bus_protocols_t *bus, uint8_t *data, uint32_t len)
{
    (void)bus;
    memset(data, 0, (len + 7) / 8);
    return 0;
}

static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    (void)bus;
}

/* pixels in internal memory go by DMA where they lie on the board; on an ESP32 all of it can */
static bool bus_sends_from(struct _dgx_bus_protocols_t *bus, const void *pixels)
{
    (void)bus;
    return ((uintptr_t)pixels & 3u) == 0;
}

static uint8_t *bus_memory(uint32_t len)
{
    return (uint8_t *)malloc(len);
}

/* the window closed and let go of with its bus, and its figures with it; SDL stays until the program ends */
static void window_free(host_window_t *w)
{
    for (host_window_t **p = &windows; *p; p = &(*p)->next)
        if (*p == w) {
            *p = w->next;
            break;
        }
    if (w->texture) SDL_DestroyTexture(w->texture);
    if (w->renderer) SDL_DestroyRenderer(w->renderer);
    if (w->window) SDL_DestroyWindow(w->window);
    free(w->protocols.buffer);
    __real_free(w->glass);
    __real_free(w);
}

static void bus_dispose(struct _dgx_bus_protocols_t *bus)
{
    window_free((host_window_t *)bus);
}

static size_t bus_buffer_len(const host_window_t *w)
{
    uint64_t len = (uint64_t)w->bus_hz / 1000u * w->bus_lines * 2u * HOST_HALF_US / 1000u / 8u;
    if (len > HOST_MAX_TRANSFER) len = HOST_MAX_TRANSFER;
    if (len < HOST_MIN_BUFFER) len = HOST_MIN_BUFFER;
    return (size_t)(len & ~(uint64_t)15);
}

/* ---- the screen ---- */

static const dgx_lcd_init_cmd_t host_init[] = {
    {HOST_COLMOD, dgx_panel_adj_colmod, 1,    0, {0x55}},
    {HOST_MADCTL, dgx_panel_adj_madctl, 1,    0, {0}   },
    {0,           0,                    0xff, 0, {0}   }
};

static size_t env_size(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? (size_t)strtoull(v, NULL, 0) : 0;
}

/* what the program has once: SDL, the variables of the environment, the report at its end */
static bool host_start(void)
{
    static bool started;
    if (started) return true;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        ESP_LOGE(TAG, "SDL: %s", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    heap.limit       = env_size("DGX_HOST_HEAP_MAX");
    heap.block_limit = env_size("DGX_HOST_BLOCK_MAX");
    shot             = getenv("DGX_HOST_SHOT");
    const char *q    = getenv("DGX_HOST_QUIT_MS");
    if (q && *q) quit_us = clock_us() + (int64_t)atol(q) * 1000;
    atexit(host_end);
    started = true;
    return true;
}

dgx_screen_t *dgx_host_screen_init(int width, int height, uint8_t color_bits, const dgx_host_screen_options_t *options)
{
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        ESP_LOGE(TAG, "a glass of %dx%d pixels: 1 to 4096 a side", width, height);
        return NULL;
    }
    if (!host_start()) return NULL;
    host_window_t *w = (host_window_t *)__real_calloc(1, sizeof(*w));
    if (!w) {
        ESP_LOGE(TAG, "no memory for a window");
        return NULL;
    }
    /* in the list from the start, so that window_free() takes it out whatever has been made of it */
    host_window_t **end = &windows;
    while (*end) end = &(*end)->next;
    *end         = w;
    w->number    = ++made;
    w->width     = width;
    w->height    = height;
    w->colmod    = 0x55;
    w->bus_hz    = options && options->bus_hz ? options->bus_hz : 40000000u;
    w->bus_lines = options && options->bus_lines ? options->bus_lines : 1u;
    if (options && options->title) snprintf(w->title, sizeof(w->title), "%s", options->title);
    else if (w->number == 1) snprintf(w->title, sizeof(w->title), "DGX");
    else snprintf(w->title, sizeof(w->title), "DGX %d", w->number);

    /* hidden until the first wait, when the windows made by then are put side by side */
    w->window = SDL_CreateWindow(w->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (w->window) w->renderer = SDL_CreateRenderer(w->window, -1, 0);
    if (w->window && !w->renderer) w->renderer = SDL_CreateRenderer(w->window, -1, SDL_RENDERER_SOFTWARE);
    if (w->renderer) w->texture = SDL_CreateTexture(w->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (w->texture) w->glass = (uint32_t *)__real_malloc((size_t)width * height * sizeof(*w->glass));
    if (!w->glass) {
        ESP_LOGE(TAG, "no window of %dx%d: %s", width, height, w->texture ? "no memory" : SDL_GetError());
        window_free(w);
        return NULL;
    }
    w->id = SDL_GetWindowID(w->window);
    SDL_SetTextureBlendMode(w->texture, SDL_BLENDMODE_NONE);
    /* the glass stretched with the window, its proportions kept */
    SDL_RenderSetLogicalSize(w->renderer, width, height);
    for (size_t i = 0; i < (size_t)width * height; ++i) w->glass[i] = 0xFF000000u;
    w->changed = true;

    dgx_bus_protocols_t *bus = &w->protocols;
    bus->bus_type            = DGX_BUS_SPI;
    bus->bus_name            = "Host";
    bus->write_command       = bus_command;
    bus->write_commands      = bus_commands;
    bus->write_data          = bus_data;
    bus->read_data           = bus_read;
    bus->write_data_async    = bus_data;
    bus->sync_write          = bus_sync;
    bus->sends_from          = bus_sends_from;
    bus->memory_allocator    = bus_memory;
    bus->dispose             = bus_dispose;
    bus->buffer_len          = bus_buffer_len(w);
    bus->buffer              = bus->memory_allocator((uint32_t)bus->buffer_len);
    w->description           = (dgx_panel_t){.name   = "Host",
                                             .width  = (uint16_t)width,
                                             .height = (uint16_t)height,
                                             .colmod = {0, 0x55, 0x66},
                                             .madctl = 0,
                                             .init   = host_init};
    dgx_screen_t *scr = bus->buffer ? dgx_panel_init(bus, &w->description, GPIO_NUM_NC, GPIO_NUM_NC, color_bits, DgxScreenRGB) : NULL;
    if (!scr) {
        window_free(w);
        return NULL;
    }
    stats_clear(w); /* the commands that started the panel are not the program's */
    ESP_LOGI(TAG, "window %d: a glass of %dx%d, %u bits; the bus of the board %.1f MHz over %u line%s, its buffer %zu bytes", w->number, width,
             height, (unsigned)color_bits, w->bus_hz / 1e6, w->bus_lines, w->bus_lines == 1 ? "" : "s", bus->buffer_len);
    return scr;
}

/*
 * The program's app_main, as a board runs it, and then the windows kept open
 * until one is closed: a board whose app_main has returned leaves its picture
 * on the displays. A program with a main() of its own has that one.
 */
extern void app_main(void) __attribute__((weak));

__attribute__((weak)) int main(void)
{
    if (!app_main) {
        fprintf(stderr, "E %s: the program has neither main() nor app_main()\n", TAG);
        return EXIT_FAILURE;
    }
    app_main();
    if (!windows) return EXIT_FAILURE;
    for (;;) dgx_host_delay(100);
}
