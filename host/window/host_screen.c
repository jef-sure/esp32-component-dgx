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
 *  The same file is the window of a program built by ESP-IDF for its linux
 *  target (ESP_PLATFORM): CMakeLists.txt of DGX builds it then, and the
 *  linker sends vTaskDelay() and app_main() here as well as malloc. The
 *  program has tasks there, each a thread of the computer, and SDL is called
 *  from one of them: the task that made the first window shows them all, in
 *  its waits and while it sends pixels. A wait of another task ends the
 *  passes of the buses that task has sent to. The memory counted is that of
 *  ESP-IDF on the computer too.
 *
 *  Variables of the environment, for runs with nobody at the computer and
 *  for a board with less memory:
 *    DGX_HOST_QUIT_MS    the program ends so many ms after the first window was made;
 *    DGX_HOST_SCRIPT     a file of the keys and the mouse button pressed and let go of, and when: see
 *                        script_read() below;
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
#include <ctype.h>
#include <errno.h>
#include <limits.h>
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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char TAG[] = "DGX HOST";

#define HOST_SWRESET 0x01u
#define HOST_CASET  0x2Au
#define HOST_RASET  0x2Bu
#define HOST_RAMWR  0x2Cu
#define HOST_MADCTL 0x36u
#define HOST_COLMOD 0x3Au
#define HOST_RAMWRC 0x3Cu

#define HOST_VSCRDEF  0x33u
#define HOST_VSCRSADD 0x37u

#define HOST_MY  0x80u
#define HOST_MX  0x40u
#define HOST_MV  0x20u
#define HOST_BGR 0x08u

/* the glass shown no more often than this; while pixels go with no wait, at least so often */
#define HOST_SHOW_US      16000
#define HOST_SHOW_BUSY_US 50000
/* the title told anew so often */
#define HOST_TITLE_US 500000
/* the stack the task that makes the windows has left at least, for SDL, on the linux target of ESP-IDF */
#define HOST_TASK_STACK (48u * 1024u)

/*
 * The buffer of the bus as dgx_spi_buffer_len() makes it on the board: a
 * half of it takes about 475 us on the wire, it is 4000 bytes at least and
 * 32000 at most, a multiple of 16. The pieces the screen sends are as long
 * as on the board then.
 */
#define HOST_HALF_US     475u
#define HOST_MIN_BUFFER  4000u
#define HOST_MAX_TRANSFER 32000u

/*
 * What the bus of the board does with what it is given while the display
 * holds it, as src/bus/spi_esp32.c and qspi_esp32.c do. By the registers of
 * the host go a command, its parameters up to HOST_SPI_SHORT bytes on SPI
 * and HOST_QSPI_SHORT on QSPI, and pixels up to HOST_DIRECT_MOST bytes, in
 * transfers of HOST_DIRECT_PIECE; longer pixels go in pieces by DMA, longer
 * parameters through the driver. A QSPI transfer starts with an opcode and
 * an address, HOST_QSPI_HEAD bits on one line; its parameters go on one
 * line too, only its pixels on four.
 */
#define HOST_SPI_SHORT    4u
#define HOST_QSPI_SHORT   64u
#define HOST_DIRECT_MOST  256u
#define HOST_DIRECT_PIECE 60u
#define HOST_QSPI_HEAD    32u
/*
 * What starting a transfer costs above its bits, by default: by the
 * registers, 0.75 us, as on the CYD (one byte took 0.95 us at 40 MHz, see
 * docs/benchmarks-en.md); a piece by DMA, 25 us, DGX_SPI_PIECE_COST_US of
 * the SPI bus, measured on an ESP32-S3.
 */
#define HOST_TRANSFER_US 0.75
#define HOST_PIECE_US    25.0

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

/* the timer of the board: microseconds since the program began, as since the chip did; the linux target of ESP-IDF declares it and has none */
__attribute__((weak)) int64_t esp_timer_get_time(void)
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
    __atomic_add_fetch(&heap.refused, 1, __ATOMIC_RELAXED);
    fprintf(stderr, "W %s: no memory for %zu bytes: %zu held, DGX_HOST_HEAP_MAX %zu, DGX_HOST_BLOCK_MAX %zu\n", TAG, size, heap.now,
            heap.limit, heap.block_limit);
    return NULL;
}

#ifdef ESP_PLATFORM
/*
 * What FreeRTOS takes for its tasks, queues and semaphores is not counted:
 * on the computer a word of a stack is 8 bytes, and every stack 8 times
 * what the same task has on the board. The C library is reached through
 * pvPortMalloc() and vPortFree() then, in the thread of the task.
 */
static __thread int in_freertos;

void *__real_pvPortMalloc(size_t size);
void  __real_vPortFree(void *p);

void *__wrap_pvPortMalloc(size_t size)
{
    ++in_freertos;
    void *p = __real_pvPortMalloc(size);
    --in_freertos;
    return p;
}

void __wrap_vPortFree(void *p)
{
    ++in_freertos;
    __real_vPortFree(p);
    --in_freertos;
}
#define HEAP_COUNTED() (!in_freertos)
#else
#define HEAP_COUNTED() 1
#endif

/* atomic: the tasks of a program for the linux target of ESP-IDF are threads, and one may be stopped in the middle for another */
static void heap_took(void *p, size_t size)
{
    if (!p) return;
    const size_t now  = __atomic_add_fetch(&heap.now, malloc_usable_size(p), __ATOMIC_RELAXED);
    size_t       most = __atomic_load_n(&heap.most, __ATOMIC_RELAXED);
    while (now > most && !__atomic_compare_exchange_n(&heap.most, &most, now, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    size_t block = __atomic_load_n(&heap.block, __ATOMIC_RELAXED);
    while (size > block && !__atomic_compare_exchange_n(&heap.block, &block, size, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

/* memory let go of; a block the C library gave by itself, as strdup() does, was never counted */
static void heap_gave(size_t size)
{
    size_t now = __atomic_load_n(&heap.now, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&heap.now, &now, now - (size < now ? size : now), true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

void *__wrap_malloc(size_t size)
{
    if (!HEAP_COUNTED()) return __real_malloc(size);
    if (!heap_lets(size, 0)) return heap_refuse(size);
    void *p = __real_malloc(size);
    heap_took(p, size);
    return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
    if (!HEAP_COUNTED()) return __real_calloc(n, size);
    if (size && n > SIZE_MAX / size) return NULL;
    if (!heap_lets(n * size, 0)) return heap_refuse(n * size);
    void *p = __real_calloc(n, size);
    heap_took(p, n * size);
    return p;
}

void *__wrap_realloc(void *p, size_t size)
{
    if (!HEAP_COUNTED()) return __real_realloc(p, size);
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
    if (p && HEAP_COUNTED()) heap_gave(malloc_usable_size(p));
    __real_free(p);
}

/* ---- what the program is told of the memory left ---- */

/*
 * As the limits have it: what DGX_HOST_HEAP_MAX leaves of what is held, the
 * largest block no larger than DGX_HOST_BLOCK_MAX. With no limit the program
 * is told of a gigabyte.
 */
#define HOST_HEAP_PLENTY ((size_t)1 << 30)

static size_t heap_left(size_t held)
{
    const size_t whole = heap.limit ? heap.limit : HOST_HEAP_PLENTY;
    return held < whole ? whole - held : 0;
}

static size_t heap_largest(void)
{
    const size_t left = heap_left(__atomic_load_n(&heap.now, __ATOMIC_RELAXED));
    return heap.block_limit && heap.block_limit < left ? heap.block_limit : left;
}

static uint32_t heap_u32(size_t size)
{
    return size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
}

#ifdef ESP_PLATFORM
/* the linux target of ESP-IDF answers a constant: the linker sends these to the window (CMakeLists.txt) */
#define HOST_ASKED(name) __wrap_##name
#else
/* declared by host/stubs/esp_heap_caps.h and host/window/stubs/esp_system.h with DGX_HOST_WINDOW */
#define HOST_ASKED(name) name
#endif

size_t HOST_ASKED(heap_caps_get_free_size)(uint32_t caps)
{
    (void)caps;
    return heap_left(__atomic_load_n(&heap.now, __ATOMIC_RELAXED));
}

size_t HOST_ASKED(heap_caps_get_minimum_free_size)(uint32_t caps)
{
    (void)caps;
    return heap_left(__atomic_load_n(&heap.most, __ATOMIC_RELAXED));
}

size_t HOST_ASKED(heap_caps_get_largest_free_block)(uint32_t caps)
{
    (void)caps;
    return heap_largest();
}

uint32_t HOST_ASKED(esp_get_free_heap_size)(void)
{
    return heap_u32(heap_left(__atomic_load_n(&heap.now, __ATOMIC_RELAXED)));
}

uint32_t HOST_ASKED(esp_get_minimum_free_heap_size)(void)
{
    return heap_u32(heap_left(__atomic_load_n(&heap.most, __ATOMIC_RELAXED)));
}

/* ---- the task that talks to SDL ---- */

#ifdef ESP_PLATFORM
/* the task that made the first window shows them all */
typedef TaskHandle_t host_task_t;
static host_task_t   this_task(void)
{
    return xTaskGetCurrentTaskHandle();
}
#else
/* one thread, the program's */
typedef int        host_task_t;
static host_task_t this_task(void)
{
    return 0;
}
#endif

/* ---- a window: the panel of a display behind its bus, and what the bus has sent ---- */

typedef struct host_window_t {
    dgx_bus_protocols_t   protocols; /* first: the bus of a screen leads to its window */
    struct host_window_t *next;
    int                   number;      /* 1 for the first window of the program */
    dgx_panel_t           description; /* what dgx_panel_init() makes the screen from */
    /* the controller, as far as the window, the scroll, MADCTL and COLMOD go */
    int       width, height;    /* the glass upright, and the memory as large */
    uint32_t *glass;            /* the memory, ARGB8888 as the window takes it, its rows those of the glass */
    uint32_t *view;             /* the glass as the scroll shows the memory, once it is scrolled */
    uint8_t   command;          /* the last command */
    uint8_t   params[6];
    int       params_n;
    uint16_t  sc, ec, sp, ep;   /* the window of the controller: its columns and pages */
    uint16_t  column, page;     /* the address of the next pixel */
    uint16_t  tfa, vsa, vsp;    /* the scroll: rows of memory above its area, rows in it, the row its first row shows */
    uint8_t   madctl, colmod;
    uint8_t   part[3];          /* the bytes of a pixel so far */
    int       part_n;
    bool      changed;          /* since the glass was shown */
    /* the bus of the board */
    uint32_t  bus_hz;
    uint8_t   bus_lines;
    double    transfer_us, piece_us; /* what a transfer by the registers and a piece by DMA cost above the wire */
    int       held;             /* QSPI: the command held back until its data come, -1 for none */
    /* the pass going on: from one wait of the task that sends to the next */
    double    us, wire_us;
    uint64_t  bytes, transfers, pieces;
    host_task_t sender;         /* the task that sent the last bytes: its wait ends the pass */
    /* the passes that sent something */
    uint64_t  passes, sum_transfers, sum_pieces;
    double    sum_us, sum_wire_us, most_us;
    uint64_t  most_bytes, most_transfers;
    /* and those since the title was told */
    uint32_t  recent;
    uint64_t  recent_transfers;
    double    recent_sum_us, recent_most_us;
    /* the window on the computer */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    uint32_t      id;
    int           turn;         /* the glass turned clockwise as the board is held, in degrees */
    bool          placed;       /* sized and put beside the others */
    char          title[64];
    int64_t       shown_us;
    /* the mouse as a finger on the glass: x, y and whether it lies there, in one word, for any task to read whole */
    uint64_t      finger;
    bool          pressed;      /* the left button of the computer went down on this window and is held */
    bool          scripted;     /* the button of the script is down on this window */
    bool          no_bus;       /* the display of the board is on no bus the window stands for: nothing is counted */
} host_window_t;

/*
 * What a program has once, as a chip has: its windows, in the order they
 * were made, and the waits that show them; the clock and the memory above.
 */
static host_window_t *windows;
static int            made;     /* windows made so far */
/*
 * The keys held, a bit a key by its SDL_Scancode: one keyboard, as a board
 * has one set of buttons, whatever window has it. Those of the computer, let
 * go of when the keyboard leaves the windows, and those of the script.
 */
static uint64_t       keys_typed[SDL_NUM_SCANCODES / 64], keys_scripted[SDL_NUM_SCANCODES / 64];
static int64_t        begun_us; /* when the first window was made: the time of DGX_HOST_QUIT_MS and DGX_HOST_SCRIPT counts from it */
static int64_t        pumped_us, titled_us, quit_us, made_us;
static bool           waited;   /* the owner has come to its first wait */
static host_task_t    owner;    /* the task that made the windows and shows them */
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
    if (command == HOST_SWRESET) {
        /* as the ILI9341 starts: no turn, 18 bits, the whole memory one area of the scroll, shown from its first row; the memory as it was */
        w->madctl = 0, w->colmod = 0x66;
        w->tfa = 0, w->vsa = (uint16_t)w->height, w->vsp = 0;
        w->sc = w->sp = 0, w->ec = (uint16_t)(w->width - 1), w->ep = (uint16_t)(w->height - 1);
        w->changed = true;
    }
}

static uint16_t param16(const host_window_t *w, int at)
{
    return (uint16_t)(w->params[at] << 8 | w->params[at + 1]);
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
            w->sc = param16(w, 0), w->ec = param16(w, 2);
        } else if (w->command == HOST_RASET && n == 4) {
            w->sp = param16(w, 0), w->ep = param16(w, 2);
        } else if (w->command == HOST_MADCTL && n == 1) {
            w->madctl = w->params[0];
        } else if (w->command == HOST_COLMOD && n == 1) {
            w->colmod = w->params[0];
        } else if (w->command == HOST_VSCRDEF && n == 6) {
            w->tfa = param16(w, 0), w->vsa = param16(w, 2);
            w->changed = true;
        } else if (w->command == HOST_VSCRSADD && n == 2) {
            w->vsp     = param16(w, 0);
            w->changed = true;
        }
    }
}

/*
 * The glass as the panel shows its memory: a row of the scroll area shows
 * the row of memory so far on from the one the area is told to start with,
 * the rows that leave at its end coming in at its start. The rows of memory
 * are those of the glass upright, whatever MADCTL turns.
 */
static const uint32_t *visible(host_window_t *w)
{
    const int first = w->tfa, count = w->vsa;
    if (count <= 0 || first + count > w->height) return w->glass;
    int shift = ((int)w->vsp - first) % count;
    if (shift < 0) shift += count;
    if (!shift) return w->glass;
    const size_t row = (size_t)w->width * sizeof(*w->glass);
    if (!w->view) w->view = (uint32_t *)__real_malloc(row * w->height);
    if (!w->view) return w->glass;
    memcpy(w->view, w->glass, row * w->height);
    for (int r = 0; r < count; ++r)
        memcpy(w->view + (size_t)(first + r) * w->width, w->glass + (size_t)(first + (r + shift) % count) * w->width, row);
    return w->view;
}

/* the size of the glass as the board is held */
static int turned_width(const host_window_t *w)
{
    return w->turn % 180 ? w->height : w->width;
}

static int turned_height(const host_window_t *w)
{
    return w->turn % 180 ? w->width : w->height;
}

/* the glass turned clockwise as the board is held, as the window shows it */
/* the pixel of the glass seen at x, y of the glass turned */
static void unturned(const host_window_t *w, int x, int y, int *gx, int *gy)
{
    const int W = w->width, H = w->height;
    *gx = x, *gy = y;
    if (w->turn == 90) *gx = y, *gy = H - 1 - x;
    else if (w->turn == 180) *gx = W - 1 - x, *gy = H - 1 - y;
    else if (w->turn == 270) *gx = W - 1 - y, *gy = x;
}

static void turn_glass(const host_window_t *w, const uint32_t *from, uint32_t *to)
{
    const int tw = turned_width(w), th = turned_height(w);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            int gx, gy;
            unturned(w, x, y, &gx, &gy);
            to[(size_t)y * tw + x] = from[(size_t)gy * w->width + gx];
        }
}

/* ---- the wire of the board ---- */

/*
 * A transfer: so many bits on one line, so many on all the data lines, by
 * the registers of the host or as a piece by DMA. Its time is that of its
 * bits at the clock of the bus and what starting it costs.
 */
static void transfer(host_window_t *w, uint32_t one_line, uint64_t all_lines, bool piece)
{
    if (w->no_bus) return;
    const double wire = ((double)one_line + (double)all_lines / w->bus_lines) * 1e6 / w->bus_hz;
    w->wire_us += wire;
    w->us += wire + (piece ? w->piece_us : w->transfer_us);
    w->bytes += (one_line + all_lines) / 8;
    if (piece) ++w->pieces;
    else ++w->transfers;
}

static bool quad(const host_window_t *w)
{
    return w->bus_lines == 4;
}

/* QSPI: a command held back that has no data goes alone, its opcode and address */
static void flush_held(host_window_t *w)
{
    if (w->held < 0) return;
    w->held = -1;
    transfer(w, HOST_QSPI_HEAD, 0, false);
}

/* pixels as dgx_spi_send_pieces() cuts them by the buffer of the bus; a QSPI piece takes a command held along */
static void transfer_pixels(host_window_t *w, uint32_t bytes)
{
    const uint32_t head   = quad(w) ? HOST_QSPI_HEAD : 0;
    const size_t   buffer = w->protocols.buffer_len;
    uint32_t       piece  = bytes <= buffer ? bytes : (uint32_t)(buffer - buffer % 12u);
    if (!piece) piece = 12u;
    w->held = -1;
    while (bytes) {
        const uint32_t now = bytes < piece ? bytes : piece;
        if (now <= HOST_DIRECT_MOST) {
            for (uint32_t left = now; left;) {
                const uint32_t part = left > HOST_DIRECT_PIECE ? HOST_DIRECT_PIECE : left;
                transfer(w, head, (uint64_t)part * 8u, false);
                left -= part;
            }
        } else {
            transfer(w, head, (uint64_t)now * 8u, true);
        }
        bytes -= now;
    }
}

/* ---- passes ---- */

static void pass_end(host_window_t *w)
{
    if (!w->transfers && !w->pieces) return;
    ++w->passes;
    w->sum_us += w->us;
    w->sum_wire_us += w->wire_us;
    w->sum_transfers += w->transfers;
    w->sum_pieces += w->pieces;
    if (w->us > w->most_us) w->most_us = w->us, w->most_bytes = w->bytes, w->most_transfers = w->transfers + w->pieces;
    ++w->recent;
    w->recent_sum_us += w->us;
    w->recent_transfers += w->transfers + w->pieces;
    if (w->us > w->recent_most_us) w->recent_most_us = w->us;
    w->us = w->wire_us = 0;
    w->bytes = w->transfers = w->pieces = 0;
}

static void stats_clear(host_window_t *w)
{
    w->us = w->wire_us = w->sum_us = w->sum_wire_us = w->most_us = w->recent_sum_us = w->recent_most_us = 0;
    w->bytes = w->transfers = w->pieces = w->passes = w->sum_transfers = w->sum_pieces = 0;
    w->most_bytes = w->most_transfers = w->recent_transfers = 0;
    w->recent                                               = 0;
}

/* ---- the windows on the computer ---- */

static void write_shot(host_window_t *w)
{
    /* the first window to the file named, the others beside it: shot-2.ppm */
    char        path[4096];
    const char *dot   = strrchr(shot, '.');
    const char *slash = strrchr(shot, '/');
    if (w->number == 1) snprintf(path, sizeof(path), "%s", shot);
    else if (dot && (!slash || dot > slash)) snprintf(path, sizeof(path), "%.*s-%d%s", (int)(dot - shot), shot, w->number, dot);
    else snprintf(path, sizeof(path), "%s-%d", shot, w->number);
    const size_t n    = (size_t)w->width * w->height;
    uint32_t    *seen = (uint32_t *)__real_malloc(n * sizeof(*seen));
    FILE        *f    = seen ? fopen(path, "wb") : NULL;
    if (!f) {
        ESP_LOGE(TAG, "no picture to %s", path);
        __real_free(seen);
        return;
    }
    turn_glass(w, visible(w), seen);
    fprintf(f, "P6\n%d %d\n255\n", turned_width(w), turned_height(w));
    for (size_t i = 0; i < n; ++i) {
        const uint8_t rgb[3] = {(uint8_t)(seen[i] >> 16), (uint8_t)(seen[i] >> 8), (uint8_t)seen[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    __real_free(seen);
}

static void tell_title(host_window_t *w)
{
    char text[256];
    int  n = snprintf(text, sizeof(text), "%s %dx%d", w->title, w->width, w->height);
    if (w->no_bus) {
        /* nothing of a bus the board does not have */
    } else if (w->recent)
        n += snprintf(text + n, sizeof(text) - n, "  |  bus %.1f ms a pass, %.1f at most, %.0f transfers", w->recent_sum_us / w->recent / 1000.0,
                      w->recent_most_us / 1000.0, (double)w->recent_transfers / w->recent);
    else
        n += snprintf(text + n, sizeof(text) - n, "  |  bus idle");
    snprintf(text + n, sizeof(text) - n, "  |  heap %.1f KB, %.1f at most", heap.now / 1024.0, heap.most / 1024.0);
    SDL_SetWindowTitle(w->window, text);
    w->recent           = 0;
    w->recent_transfers = 0;
    w->recent_sum_us    = 0;
    w->recent_most_us   = 0;
}

/*
 * What the program has done, when it ends. SDL is not quit: the system takes
 * the windows away with the program, and by the time exit() comes to this,
 * the graphics driver of the computer may have unloaded parts of itself in a
 * handler of its own, registered later and so run earlier. SDL_Quit() then
 * calls into them: the EGL of NVIDIA does that under SDL_VIDEODRIVER=offscreen.
 */
static void host_end(void)
{
    for (host_window_t *w = windows; w; w = w->next) {
        pass_end(w);
        if (shot && *shot) write_shot(w);
        if (w->no_bus) {
            fprintf(stderr, "I %s: window %d, %dx%d, no bus counted\n", TAG, w->number, w->width, w->height);
            continue;
        }
        const double passes = w->passes ? (double)w->passes : 1.0;
        fprintf(stderr,
                "I %s: window %d, %dx%d, %s at %.1f MHz: passes that sent something: %llu; a pass %.2f ms on average, %.2f on the wire, "
                "%.1f transfers by the registers at %.2f us and %.1f pieces by DMA at %.1f us; %.2f ms at most (%llu bytes in %llu "
                "transfers)\n",
                TAG, w->number, w->width, w->height, quad(w) ? "QSPI" : "SPI", w->bus_hz / 1e6, (unsigned long long)w->passes,
                w->sum_us / passes / 1000.0, w->sum_wire_us / passes / 1000.0, w->sum_transfers / passes, w->transfer_us,
                w->sum_pieces / passes, w->piece_us, w->most_us / 1000.0, (unsigned long long)w->most_bytes,
                (unsigned long long)w->most_transfers);
    }
    fprintf(stderr, "I %s: memory: %zu bytes held now, %zu at most at once, the largest block %zu", TAG, heap.now, heap.most, heap.block);
    if (heap.refused) fprintf(stderr, "; %u allocations refused", heap.refused);
    fputc('\n', stderr);
}

/*
 * The windows not shown yet, side by side in the middle of the screen of
 * the computer, the glass of each the same whole number of times as large,
 * as large as they all fit. Those shown before stay where they are.
 */
static void place(void)
{
    const int gap   = 16;
    int       total = 0, high = 0, n = 0;
    for (host_window_t *w = windows; w; w = w->next) {
        if (w->placed) continue;
        total += turned_width(w);
        if (turned_height(w) > high) high = turned_height(w);
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
        const int tw = turned_width(w) * zoom, th = turned_height(w) * zoom;
        SDL_SetWindowSize(w->window, tw, th);
        SDL_SetWindowPosition(w->window, x, usable.y + (usable.h - th) / 2);
        SDL_ShowWindow(w->window);
        ESP_LOGI(TAG, "window %d: the glass %d times as large, %dx%d", w->number, zoom, tw, th);
        x += tw + gap;
        w->placed  = true;
        w->changed = true;
    }
}

/* the events of the windows read: a closed window ends the program, as there is one board */
static host_window_t *window_of_id(uint32_t id)
{
    for (host_window_t *w = windows; w; w = w->next)
        if (w->id == id) return w;
    return NULL;
}

/* the finger of a window on the pixel gx, gy of the glass upright; outside the glass, on its edge nearest to it */
static void finger_put(host_window_t *w, int gx, int gy)
{
    gx = gx < 0 ? 0 : gx >= w->width ? w->width - 1 : gx;
    gy = gy < 0 ? 0 : gy >= w->height ? w->height - 1 : gy;
    __atomic_store_n(&w->finger, (uint64_t)1 << 32 | (uint64_t)(uint16_t)gy << 16 | (uint16_t)gx, __ATOMIC_RELEASE);
}

/*
 * The mouse on a window, at x, y of the glass as the window shows it: SDL
 * gives the events of a window with a logical size in its pixels, the
 * stretch and the bars beside it taken away.
 */
static void finger_at(host_window_t *w, int x, int y)
{
    int gx, gy;
    unturned(w, x, y, &gx, &gy);
    finger_put(w, gx, gy);
}

/* the finger gone once neither the mouse of the computer nor the script holds it */
static void finger_off(host_window_t *w)
{
    if (!w->pressed && !w->scripted) __atomic_store_n(&w->finger, 0, __ATOMIC_RELEASE);
}

/* a key down or up; held on, as the computer repeats it, it stays down */
static void key_set(uint64_t *keys, unsigned code, bool down)
{
    if (code >= SDL_NUM_SCANCODES) return;
    const uint64_t bit = (uint64_t)1 << (code % 64);
    if (down) __atomic_or_fetch(&keys[code / 64], bit, __ATOMIC_RELEASE);
    else __atomic_and_fetch(&keys[code / 64], ~bit, __ATOMIC_RELEASE);
}

/* whether a window of the program has the keyboard now */
static bool keyboard_here(void)
{
    SDL_Window *focus = SDL_GetKeyboardFocus();
    return focus && window_of_id(SDL_GetWindowID(focus));
}

/*
 * The events of the windows read: the mouse, the keys, and a closed window,
 * which ends the program, as there is one board. A window that loses the
 * keyboard to another of the program keeps the keys held: they go up on
 * that one. Shown, a window may take the keyboard by itself.
 */
static void pump(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        host_window_t *w;
        switch (event.type) {
        case SDL_QUIT:
            exit(EXIT_SUCCESS);
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_CLOSE) exit(EXIT_SUCCESS);
            if (!(w = window_of_id(event.window.windowID))) break;
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                w->pressed = false;
                finger_off(w);
                if (!keyboard_here())
                    for (size_t i = 0; i < sizeof(keys_typed) / sizeof(*keys_typed); ++i) __atomic_store_n(&keys_typed[i], 0, __ATOMIC_RELEASE);
            }
            w->changed = true; /* uncovered, stretched: drawn again */
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            key_set(keys_typed, event.key.keysym.scancode, event.type == SDL_KEYDOWN);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button != SDL_BUTTON_LEFT || !(w = window_of_id(event.button.windowID))) break;
            w->pressed = true;
            finger_at(w, event.button.x, event.button.y);
            break;
        case SDL_MOUSEMOTION:
            if ((w = window_of_id(event.motion.windowID)) && w->pressed) finger_at(w, event.motion.x, event.motion.y);
            break;
        case SDL_MOUSEBUTTONUP:
            /* let go wherever the button is: the window it went down on may not have it now */
            if (event.button.button != SDL_BUTTON_LEFT) break;
            for (w = windows; w; w = w->next)
                if (w->pressed) w->pressed = false, finger_off(w);
            break;
        default:
            break;
        }
    }
}

/* ---- the script: the keys and the mouse, and when, for runs with nobody at the computer ---- */

enum { STEP_KEY_DOWN, STEP_KEY_UP, STEP_MOUSE_DOWN, STEP_MOUSE_MOVE, STEP_MOUSE_UP };

typedef struct {
    int64_t  ms;     /* after the first window was made */
    int      window; /* the number of the window of the mouse: 1 for the first */
    int      line;   /* of the file */
    uint8_t  what;
    uint16_t key;    /* SDL_Scancode */
    int16_t  x, y;   /* the pixel of the glass upright the mouse is on */
} host_step_t;

static host_step_t *steps;
static int          steps_n, steps_done;
static const char  *script_name;

static void skip_blanks(char **s)
{
    while (isspace((unsigned char)**s)) ++*s;
}

/* the word at *s taken, and the blanks after it, if it is the one asked for */
static bool word(char **s, const char *w)
{
    const size_t n = strlen(w);
    if (strncmp(*s, w, n) || ((*s)[n] && !isspace((unsigned char)(*s)[n]))) return false;
    *s += n;
    skip_blanks(s);
    return true;
}

/* a whole number at *s taken, and the blanks after it */
static bool number(char **s, long lo, long hi, long *v)
{
    char *end;
    errno        = 0;
    const long n = strtol(*s, &end, 10);
    if (end == *s || errno || n < lo || n > hi || (*end && !isspace((unsigned char)*end))) return false;
    *v = n, *s = end;
    skip_blanks(s);
    return true;
}

/*
 * The steps of a script, a line each, in the order they come:
 *
 *   # Right, then Shift with it; a line of # and a blank one are nothing
 *   500 key down Right
 *   900 key up Right
 *   900 key down Left Shift
 *   1000 mouse down 120 40
 *   1100 mouse move 140 40
 *   1200 mouse up
 *   1500 window 2 mouse down 5 5
 *
 * The number first is the time, in ms after the first window was made, as
 * that of DGX_HOST_QUIT_MS; a line comes no earlier than the one above it.
 * A key has the name SDL gives it. The keys are those of the program, as
 * the keyboard is, whatever window has it. The mouse is on the first window
 * unless another is named, at a pixel of the glass upright, as the fingers
 * are told; outside the glass the finger lies on its edge. It moves and goes
 * up on the window it went down on. NULL for a script that is right, else
 * what is wrong with it, and the line in *line.
 */
static const char *script_read(FILE *f, int *line)
{
    char  text[256];
    long  last = 0, x = 0, y = 0;
    int   down = 0; /* the window the button is down on, 0 for none */
    *line      = 0;
    while (fgets(text, sizeof(text), f)) {
        ++*line;
        if (!strchr(text, '\n') && !feof(f)) return "a line longer than 254 characters";
        char *s = text;
        skip_blanks(&s);
        for (char *e = s + strlen(s); e > s && isspace((unsigned char)e[-1]);) *--e = 0;
        if (!*s || *s == '#') continue;
        host_step_t step = {.window = 0, .line = *line};
        long        v;
        if (!number(&s, 0, LONG_MAX / 1000 - 1, &v)) return "first the time: ms after the first window was made";
        if (v < last) return "a time earlier than that of the line above";
        step.ms = last = v;
        if (word(&s, "window")) {
            if (!number(&s, 1, INT_MAX, &v)) return "window and its number: 1 for the first one";
            step.window = (int)v;
        }
        if (word(&s, "key")) {
            if (step.window) return "a key goes down for the program, on no window";
            if (word(&s, "down")) step.what = STEP_KEY_DOWN;
            else if (word(&s, "up")) step.what = STEP_KEY_UP;
            else return "key down or key up, then the name of the key";
            const SDL_Scancode code = *s ? SDL_GetScancodeFromName(s) : SDL_SCANCODE_UNKNOWN;
            if (code == SDL_SCANCODE_UNKNOWN) return "no key of that name: the names are SDL's, as Left, Space, A, Left Shift";
            step.key = (uint16_t)code;
        } else if (word(&s, "mouse")) {
            if (!step.window) step.window = 1;
            if (word(&s, "down")) step.what = STEP_MOUSE_DOWN;
            else if (word(&s, "move")) step.what = STEP_MOUSE_MOVE;
            else if (word(&s, "up")) step.what = STEP_MOUSE_UP;
            else return "mouse down, mouse move or mouse up";
            if (step.what == STEP_MOUSE_DOWN && down) return "mouse down with the button down";
            if (step.what != STEP_MOUSE_DOWN && !down) return "mouse move or mouse up with the button up";
            if (step.what != STEP_MOUSE_DOWN && step.window != down) return "mouse move or mouse up on another window than the button went down on";
            if (step.what != STEP_MOUSE_UP && (!number(&s, INT16_MIN, INT16_MAX, &x) || !number(&s, INT16_MIN, INT16_MAX, &y)))
                return "mouse down and mouse move take x and y, a pixel of the glass upright";
            if (*s) return "something after the mouse";
            if (step.what != STEP_MOUSE_MOVE) down = step.what == STEP_MOUSE_DOWN ? step.window : 0;
            step.x = (int16_t)x, step.y = (int16_t)y;
        } else {
            return "key or mouse after the time";
        }
        if (!(steps_n & (steps_n + 1))) {
            /* room for 1, 3, 7... */
            host_step_t *more = (host_step_t *)__real_realloc(steps, sizeof(*steps) * (size_t)(steps_n * 2 + 2));
            if (!more) return "no memory for the script";
            steps = more;
        }
        steps[steps_n++] = step;
    }
    return ferror(f) ? strerror(errno) : NULL;
}

/* DGX_HOST_SCRIPT read: a script that is wrong ends the program, as a run with nobody at the computer goes wrong without it */
static void script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "DGX_HOST_SCRIPT %s: %s", path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    int         line;
    const char *wrong = script_read(f, &line);
    fclose(f);
    if (wrong) {
        ESP_LOGE(TAG, "DGX_HOST_SCRIPT %s:%d: %s", path, line, wrong);
        exit(EXIT_FAILURE);
    }
    script_name = path;
    ESP_LOGI(TAG, "DGX_HOST_SCRIPT %s: %d steps", path, steps_n);
}

static host_window_t *window_numbered(int number)
{
    for (host_window_t *w = windows; w; w = w->next)
        if (w->number == number) return w;
    return NULL;
}

/*
 * The steps whose time has come, read as those of the keyboard and the
 * mouse of the computer are, in the same waits. They are the script's: a
 * window that loses the keyboard or the mouse lets go of none of them.
 */
static void script_play(int64_t now)
{
    while (steps_done < steps_n && now >= begun_us + steps[steps_done].ms * 1000) {
        const host_step_t *s = &steps[steps_done++];
        if (s->what == STEP_KEY_DOWN || s->what == STEP_KEY_UP) {
            key_set(keys_scripted, s->key, s->what == STEP_KEY_DOWN);
            continue;
        }
        host_window_t *w = window_numbered(s->window);
        if (!w) {
            ESP_LOGE(TAG, "DGX_HOST_SCRIPT %s:%d: no window %d at %lld ms", script_name, s->line, s->window, (long long)s->ms);
            exit(EXIT_FAILURE);
        }
        if (s->what == STEP_MOUSE_UP) {
            w->scripted = false;
            finger_off(w);
        } else {
            w->scripted = true;
            finger_put(w, s->x, s->y);
        }
    }
}

/* every window shown, the ones whose glass has changed, when it is time */
static void show(int64_t every_us)
{
    if (!windows) return;
    place();
    script_play(clock_us());
    pump();
    const int64_t now = clock_us();
    for (host_window_t *w = windows; w; w = w->next) {
        if (!w->changed || now - w->shown_us < every_us) continue;
        SDL_UpdateTexture(w->texture, NULL, visible(w), w->width * (int)sizeof(*w->glass));
        SDL_RenderClear(w->renderer);
        if (w->turn) {
            /* the glass turned about the middle of the window, which has its sides the other way at 90 and 270 */
            const SDL_Rect to = {(turned_width(w) - w->width) / 2, (turned_height(w) - w->height) / 2, w->width, w->height};
            SDL_RenderCopyEx(w->renderer, w->texture, NULL, &to, w->turn, NULL, SDL_FLIP_NONE);
        } else {
            SDL_RenderCopy(w->renderer, w->texture, NULL, NULL);
        }
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

/* a wait of a task: the passes of the buses it has sent to end; from the first wait of the owner on the windows are shown */
static void wait_begins(void)
{
    const host_task_t task = this_task();
    for (host_window_t *w = windows; w; w = w->next)
        if (w->sender == task) pass_end(w);
    if (task == owner) waited = true;
}

#ifdef ESP_PLATFORM
void __real_vTaskDelay(const TickType_t ticks);
void __real_app_main(void);

/* vTaskDelay() of the program, of DGX and of ESP-IDF: the owner of the windows shows them while it waits, a few ms at a time */
void __wrap_vTaskDelay(const TickType_t ticks)
{
    wait_begins();
    if (!windows || this_task() != owner) {
        __real_vTaskDelay(ticks);
        return;
    }
    const TickType_t step = pdMS_TO_TICKS(4) ? pdMS_TO_TICKS(4) : 1;
    TickType_t       left = ticks;
    do {
        show(HOST_SHOW_US);
        const TickType_t now = left < step ? left : step;
        __real_vTaskDelay(now);
        left -= now;
    } while (left);
}

/* app_main() as ESP-IDF calls it in its main task, then the windows kept open until one is closed, as with make */
void __wrap_app_main(void)
{
    __real_app_main();
    if (!windows || this_task() != owner) return;
    for (;;) __wrap_vTaskDelay(pdMS_TO_TICKS(100) ? pdMS_TO_TICKS(100) : 1);
}

/* the pad of a pin of host/esp-idf/include/driver/gpio.h: the linux target declares it and does not have it */
__attribute__((weak)) void esp_rom_gpio_pad_select_gpio(uint32_t iopad_num)
{
    (void)iopad_num;
}
#else
/* the wait of the program: vTaskDelay() and dgx_delay() of host/stubs with DGX_HOST_WINDOW */
void dgx_host_delay(uint32_t ms)
{
    wait_begins();
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
#endif

/* ---- the bus ---- */

/*
 * Something sent by the task: its wait ends the pass. The windows are shown
 * while pixels go, by the owner, once it has waited or once no window has
 * been made for a while: a program that never waits still has its windows.
 */
static void sent(host_window_t *w)
{
    const host_task_t task = this_task();
    w->sender              = task;
    if (task != owner) return;
    const int64_t now = clock_us();
    if (now - pumped_us >= HOST_SHOW_BUSY_US && (waited || now - made_us >= HOST_SHOW_BUSY_US)) show(HOST_SHOW_BUSY_US);
}

static void bus_command(struct _dgx_bus_protocols_t *bus, uint8_t command)
{
    host_window_t *w = (host_window_t *)bus;
    panel_command(w, command);
    if (quad(w)) {
        flush_held(w);
        w->held = command;
    } else {
        transfer(w, 8, 0, false);
    }
    sent(w);
}

/* SPI: the bytes in one transfer of the driver; QSPI: every byte a command of its own */
static void bus_commands(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    host_window_t *w = (host_window_t *)bus;
    for (uint32_t i = 0; i < len / 8; ++i) {
        panel_command(w, data[i]);
        if (quad(w)) flush_held(w), w->held = data[i];
    }
    if (quad(w)) flush_held(w);
    else transfer(w, len, 0, true);
    sent(w);
}

/*
 * Parameters, or pixels sent and waited for. SPI: up to four bytes by the
 * registers, more through the driver, in pieces of the buffer. QSPI: with
 * the command held back, all on one line, by the registers up to 64 bytes;
 * with no command they are dropped, as on the board.
 */
static void bus_data(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    host_window_t *w     = (host_window_t *)bus;
    const uint32_t bytes = len / 8;
    if (quad(w)) {
        if (w->held < 0) return;
        w->held = -1;
        panel_data(w, data, bytes);
        transfer(w, HOST_QSPI_HEAD + len, 0, bytes > HOST_QSPI_SHORT);
    } else {
        panel_data(w, data, bytes);
        if (bytes <= HOST_SPI_SHORT) {
            transfer(w, len, 0, false);
        } else {
            for (uint32_t left = bytes; left;) {
                const uint32_t piece = left < w->protocols.buffer_len ? left : (uint32_t)w->protocols.buffer_len;
                transfer(w, piece * 8u, 0, true);
                left -= piece;
            }
        }
    }
    sent(w);
}

static void bus_data_async(struct _dgx_bus_protocols_t *bus, const uint8_t *data, uint32_t len)
{
    host_window_t *w = (host_window_t *)bus;
    panel_data(w, data, len / 8);
    transfer_pixels(w, len / 8);
    sent(w);
}

static uint32_t bus_read(struct _dgx_bus_protocols_t *bus, uint8_t *data, uint32_t len)
{
    (void)bus;
    memset(data, 0, (len + 7) / 8);
    return 0;
}

/* QSPI: a command held back goes alone now, but for the memory write, which waits for its pixels */
static void bus_sync(struct _dgx_bus_protocols_t *bus)
{
    host_window_t *w = (host_window_t *)bus;
    if (quad(w) && w->held >= 0 && w->held != HOST_RAMWR) flush_held(w);
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
    __real_free(w->view);
    __real_free(w->glass);
    __real_free(w);
}

static void bus_dispose(struct _dgx_bus_protocols_t *bus)
{
    window_free((host_window_t *)bus);
}

/* the buffer as dgx_spi_buffer_len() makes it, the program's limit too: in whole words, a multiple of 16 bytes when it can */
static size_t bus_buffer_len(const host_window_t *w, uint32_t most)
{
    uint64_t len = (uint64_t)w->bus_hz / 1000u * w->bus_lines * 2u * HOST_HALF_US / 1000u / 8u;
    if (len > HOST_MAX_TRANSFER) len = HOST_MAX_TRANSFER;
    if (len < HOST_MIN_BUFFER) len = HOST_MIN_BUFFER;
    if (most && len > most) len = most;
    const uint64_t words = len & ~(uint64_t)3;
    len &= ~(uint64_t)15;
    if (!len) len = words ? words : 4u;
    return (size_t)len;
}

/* ---- the screen ---- */

static const dgx_lcd_init_cmd_t host_init[] = {
    {HOST_SWRESET, 0,                   0,    0, {0}   },
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
    begun_us         = clock_us();
    const char *q    = getenv("DGX_HOST_QUIT_MS");
    if (q && *q) quit_us = begun_us + (int64_t)atol(q) * 1000;
    const char *script = getenv("DGX_HOST_SCRIPT");
    if (script && *script) script_load(script);
    atexit(host_end);
    started = true;
    return true;
}

/* a cost of the options: 0 for the default, below 0 for none */
static double cost_of(float asked, double otherwise)
{
    return asked > 0 ? asked : asked < 0 ? 0.0 : otherwise;
}

dgx_screen_t *dgx_host_screen_init(int width, int height, uint8_t color_bits, const dgx_host_screen_options_t *options)
{
    const dgx_host_screen_options_t none = {0};
    if (!options) options = &none;
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        ESP_LOGE(TAG, "a glass of %dx%d pixels: 1 to 4096 a side", width, height);
        return NULL;
    }
    if (options->bus_lines > 1 && options->bus_lines != 4) {
        ESP_LOGE(TAG, "a bus of %u data lines: 1 for SPI, 4 for QSPI", (unsigned)options->bus_lines);
        return NULL;
    }
    if (options->turn % 90 || options->turn >= 360) {
        ESP_LOGE(TAG, "the glass turned by %u degrees: 0, 90, 180 or 270", (unsigned)options->turn);
        return NULL;
    }
    if (windows && this_task() != owner) {
        ESP_LOGE(TAG, "the windows are made by the task that made the first one: it shows them");
        return NULL;
    }
#ifdef ESP_PLATFORM
    /*
     * SDL goes deep into the graphics driver of the computer, in the stack of
     * the task: 32 KB took the program down in the driver of an NVIDIA card,
     * 64 KB did not. Refused here rather than there.
     */
    const size_t stack = (size_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    if (stack < HOST_TASK_STACK) {
        ESP_LOGE(TAG, "the task that makes the windows has %zu bytes of stack left, SDL needs %u: give it more, "
                      "CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384 for app_main (a word of a stack is 8 bytes on the computer)",
                 stack, (unsigned)HOST_TASK_STACK);
        return NULL;
    }
#endif
    if (!host_start()) return NULL;
    if (!windows) owner = this_task();
    host_window_t *w = (host_window_t *)__real_calloc(1, sizeof(*w));
    if (!w) {
        ESP_LOGE(TAG, "no memory for a window");
        return NULL;
    }
    /* in the list from the start, so that window_free() takes it out whatever has been made of it */
    host_window_t **end = &windows;
    while (*end) end = &(*end)->next;
    *end           = w;
    made_us        = clock_us(); /* and again once it is made: its panel starts with no window shown */
    w->number      = ++made;
    w->width       = width;
    w->height      = height;
    w->colmod      = 0x55;
    w->held        = -1;
    w->bus_hz      = options->bus_hz ? options->bus_hz : 40000000u;
    w->bus_lines   = options->bus_lines ? options->bus_lines : 1u;
    w->transfer_us = cost_of(options->transfer_us, HOST_TRANSFER_US);
    w->piece_us    = cost_of(options->piece_us, HOST_PIECE_US);
    w->turn        = options->turn;
    w->no_bus      = options->no_bus;
    if (options->title) snprintf(w->title, sizeof(w->title), "%s", options->title);
    else if (w->number == 1) snprintf(w->title, sizeof(w->title), "DGX");
    else snprintf(w->title, sizeof(w->title), "DGX %d", w->number);

    /* hidden until it is shown first, when the windows made by then are put side by side */
    w->window = SDL_CreateWindow(w->title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, turned_width(w), turned_height(w),
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
    SDL_RenderSetLogicalSize(w->renderer, turned_width(w), turned_height(w));
    for (size_t i = 0; i < (size_t)width * height; ++i) w->glass[i] = 0xFF000000u;
    w->changed = true;

    dgx_bus_protocols_t *bus = &w->protocols;
    bus->bus_type            = quad(w) ? DGX_BUS_QSPI : DGX_BUS_SPI;
    bus->bus_name            = quad(w) ? "Host QSPI" : "Host SPI";
    bus->write_command       = bus_command;
    bus->write_commands      = bus_commands;
    bus->write_data          = bus_data;
    bus->read_data           = bus_read;
    bus->write_data_async    = bus_data_async;
    bus->sync_write          = bus_sync;
    bus->sends_from          = bus_sends_from;
    bus->memory_allocator    = bus_memory;
    bus->dispose             = bus_dispose;
    bus->buffer_len          = bus_buffer_len(w, options->buffer_max);
    bus->buffer              = bus->memory_allocator((uint32_t)bus->buffer_len);
    /* the scroll of the panel along the rows of its memory, the rows of the glass, as the ILI9341 has it */
    w->description = (dgx_panel_t){.name             = "Host",
                                   .width            = (uint16_t)width,
                                   .height           = (uint16_t)height,
                                   .colmod           = {0, 0x55, 0x66},
                                   .madctl           = 0,
                                   .scroll_lines     = (uint16_t)height,
                                   .scroll_area_cmd  = HOST_VSCRDEF,
                                   .scroll_start_cmd = HOST_VSCRSADD,
                                   .init             = host_init};
    dgx_screen_t *scr = bus->buffer ? dgx_panel_init(bus, &w->description, GPIO_NUM_NC, GPIO_NUM_NC, color_bits, DgxScreenRGB) : NULL;
    if (!scr) {
        window_free(w);
        return NULL;
    }
    stats_clear(w); /* the commands that started the panel are not the program's */
    made_us = clock_us();
    if (w->no_bus)
        ESP_LOGI(TAG, "window %d: a glass of %dx%d, %u bits; no bus of the board counted", w->number, width, height, (unsigned)color_bits);
    else
        ESP_LOGI(TAG, "window %d: a glass of %dx%d, %u bits; the bus of the board %s at %.1f MHz, its buffer %zu bytes", w->number, width,
                 height, (unsigned)color_bits, quad(w) ? "QSPI" : "SPI", w->bus_hz / 1e6, bus->buffer_len);
    return scr;
}

/* the window of a screen of dgx_host_screen_init(), NULL for another screen */
static host_window_t *window_of_screen(const dgx_screen_t *scr)
{
    if (!scr || scr->screen_subtype != DgxPhysicalScreenWithBus) return NULL;
    dgx_bus_protocols_t *bus = ((const dgx_screen_with_bus_t *)scr)->bus;
    return bus && bus->dispose == bus_dispose ? (host_window_t *)bus : NULL;
}

int dgx_host_screen_fingers(const dgx_screen_t *scr, dgx_host_finger_t *fingers, int max)
{
    host_window_t *w = window_of_screen(scr);
    if (!w || !fingers || max < 1) return 0;
    const uint64_t finger = __atomic_load_n(&w->finger, __ATOMIC_ACQUIRE);
    if (!(finger >> 32 & 1u)) return 0;
    fingers[0] = (dgx_host_finger_t){.x = (int16_t)(finger & 0xFFFFu), .y = (int16_t)(finger >> 16 & 0xFFFFu), .id = 0};
    return 1;
}

int dgx_host_keys(uint16_t *keys, int max)
{
    if (!keys) return 0;
    int n = 0;
    for (int i = 0; i < SDL_NUM_SCANCODES / 64 && n < max; ++i) {
        uint64_t held = __atomic_load_n(&keys_typed[i], __ATOMIC_ACQUIRE) | __atomic_load_n(&keys_scripted[i], __ATOMIC_ACQUIRE);
        for (; held && n < max; held &= held - 1) keys[n++] = (uint16_t)(i * 64 + __builtin_ctzll(held));
    }
    return n;
}

#ifndef ESP_PLATFORM
/*
 * The program's app_main, as a board runs it, and then the windows kept open
 * until one is closed: a board whose app_main has returned leaves its picture
 * on the displays. A program with a main() of its own has that one; under
 * ESP-IDF the main() is its own.
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
#endif
