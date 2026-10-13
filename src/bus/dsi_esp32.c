/*
 * dsi_esp32.c
 *
 *  Created on: 11.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  MIPI-DSI bus on the esp_lcd driver. The display is in video mode: the
 *  chip sends it a frame buffer over and over. The bus takes the commands
 *  of the window and the pixels itself and puts them into that buffer.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_attr.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "bus/dgx_dsi_esp32.h"
#include "dgx_fb_area_priv.h"
#include "dgx_fb_flip_priv.h"
#include "dgx_fb_tiles_priv.h"
#include "dgx_screen_functions.h"
#include "dgx_screen_with_bus.h"

#define DGX_DSI_BUFFER_LEN 4096

/*
 * The 2D DMA of the chip moves a rectangle from one picture into another
 * past the processor and its cache, twice as fast as the display reads, and
 * can exchange the two bytes of a 16-bit pixel on the way. Only the private
 * interface of IDF gives that: the public one, esp_async_color_convert.h of
 * IDF 6.0.3 and 6.1, copies and converts colors but does not exchange bytes.
 */
#if defined(__has_include) && SOC_DMA2D_SUPPORTED && CONFIG_DGX_DSI_HARDWARE_COPY
#if __has_include("esp_private/dma2d.h")
#include "esp_private/dma2d.h"
#include "hal/dma2d_types.h"
#include "soc/dma2d_channel.h"
#include "soc/dma2d_struct.h"
#define DGX_DSI_HARDWARE_COPY 1
#endif
#endif

/*
 * The pixel processing accelerator of the chip fills a rectangle of a
 * picture with a color past the processor and its cache, four times as fast.
 * This one has a public interface.
 */
#if defined(__has_include) && SOC_PPA_SUPPORTED && CONFIG_DGX_DSI_HARDWARE_COPY
#if __has_include("driver/ppa.h")
#include "driver/ppa.h"
#define DGX_DSI_HARDWARE_FILL 1
/*
 * The same accelerator mixes two colors by an 8-bit picture of how much of the second there is: what a
 * table of two colors does for an 8-bit screen. It rounds in its own way, so a color between the two
 * may differ from the table by a step of a channel; a program that wants the colors of its table and
 * nothing else says so when it sends the screen, DGX_LUT_EXACT.
 */
#define DGX_DSI_HARDWARE_MIX 1
#endif
#endif

/*
 * The driver of the display tells where it takes the frame buffer for the
 * next frame by on_frame_buf_complete, on every chip: IDF 5.5.5, 6.0.3 and
 * 6.1 have it, the release branches got it at different times. So it is
 * told by the header and not by the number of the IDF: CMakeLists.txt looks
 * for the name there and defines DGX_DSI_FRAME_BUF_COMPLETE.
 */

/*
 * The end of a frame is told where the display takes the frame buffer for
 * the next one: where the last row has been read. With the older name,
 * on_refresh_done, a chip of revision 3 and later tells it at the vertical
 * sync that follows, which is later.
 */
#if DGX_DSI_FRAME_BUF_COMPLETE || !(defined(CONFIG_ESP32P4_REV_MIN_FULL) && CONFIG_ESP32P4_REV_MIN_FULL >= 300)
#define DGX_DSI_REFRESH_AT_SWITCH 1
#endif

/*
 * What is written to the frame buffer lies in the cache of the chip until it
 * is brought out. Past a part of the size of that cache the chip starts to
 * put lines out on its own, in an order of its own, and a long drawing shows
 * with holes that fill later. So when pixels and rectangles are drawn, the
 * bus brings out what was written before it comes to that, and the drawing
 * shows in the order it is made. Pixels sent through the window go one
 * after another in memory and leave the cache in that order by themselves;
 * bringing them out by force only costs time, a tenth on a whole screen.
 */
#ifdef CONFIG_CACHE_L2_CACHE_SIZE
#define DGX_DSI_SHOW_WEIGHT (CONFIG_CACHE_L2_CACHE_SIZE / 4)
#else
#define DGX_DSI_SHOW_WEIGHT (32 * 1024)
#endif

typedef struct _dgx_dsi_bus_t dgx_dsi_bus_t;
#if DGX_DSI_HARDWARE_COPY || DGX_DSI_HARDWARE_FILL
static bool dgx_dsi_wait_scan_outside(dgx_dsi_bus_t *bus, uint32_t first, uint32_t last);
#endif

/*
 * With two frame buffers a large piece a finished picture lacks is not
 * copied when the picture is finished. The display reads a frame buffer row
 * after row, so the piece is in time when it is there before the display
 * comes to its rows: it is copied after the display has taken the picture,
 * by the 2D DMA, which writes twice as fast as the display reads and is
 * started in the interrupt that tells the end of the frame. Until then
 * another batch may draw over the piece, and then it is not copied at all:
 * so tasks that redraw each its own part of the screen cost no copy.
 *
 * The pieces go one after another from the top. Two at once would each be
 * hardly faster than the display reads: 83 KB in a millisecond against 77,
 * measured on an ESP32-P4. A transfer or a fill a task starts meanwhile
 * slows a piece down the same way. So it is reckoned beforehand whether the
 * pieces stay ahead of the display when they are slowed down so; when they
 * do not, such a transfer waits until they are all there, otherwise only
 * what would write over a piece waits for that piece.
 *
 * How fast the 2D DMA copies depends on the chip, its memory and what else
 * runs, so it is not assumed: every piece that was copied with nothing else
 * moved is timed, and the slowest of the last ones is what is reckoned
 * with. How fast the display reads follows from the time of a frame. Until
 * a piece has been timed the pieces are taken as needing all the speed.
 */
/* a piece is timed when it is at least this large: a small one tells more of the starting than of the copying */
#define DGX_DSI_LATE_TIMED_FROM 65536u
/* a piece copied while something else is moved takes this many halves of its time: twice as long was measured, some more is taken */
#define DGX_DSI_LATE_SHARED_HALVES 5u
#if DGX_DSI_HARDWARE_COPY
#define DGX_DSI_LATE_PIECES 6

typedef struct {
    dgx_dsi_bus_t *bus;
    uint8_t        piece;
} dgx_dsi_late_context_t;

typedef struct {
    uint16_t left, top, width, rows; /* on the panel */
} dgx_dsi_late_piece_t;

typedef struct {
    dma2d_descriptor_t    *desc;       /* two a piece, of the picture read and of the picture written */
    uint8_t               *trans;      /* a transfer a piece */
    uint32_t               trans_size; /* of one of them */
    dma2d_trans_config_t   config[DGX_DSI_LATE_PIECES];
    dgx_dsi_late_context_t context[DGX_DSI_LATE_PIECES];
    dgx_dsi_late_piece_t   piece[DGX_DSI_LATE_PIECES];
    uint8_t                count;      /* pieces left for later when the picture was last finished */
    uint8_t                into;       /* the frame buffer they go into */
    bool                   alone;      /* they stay ahead of the display only when nothing else is moved meanwhile */
    volatile bool          shared;     /* something else was moved while they were copied: their time tells nothing */
    uint32_t               piece_us;   /* when the piece being copied was started */
    uint32_t               mb_us;      /* microseconds the 2D DMA takes for a megabyte, the slowest of the last pieces; 0 until one is timed */
    /*
     * Set when the display is told of the picture; whoever takes it
     * away owns the pieces: the interrupt where a frame ends, which
     * starts them, or a task that draws into the same picture.
     */
    _Atomic uint32_t       armed;
    volatile uint8_t       left;       /* a bit a piece that is not copied yet */
    SemaphoreHandle_t      step;       /* given when a piece has been copied */
    uint32_t               from_us;    /* when the frame ended they were started at */
    /* counted since the start */
    uint32_t               bytes;      /* copied so */
    uint32_t               behind;     /* pieces that ended when the display had read past them */
    uint32_t               waits;      /* times drawing waited for a piece */
    uint32_t               wait_us;    /* and how long in all */
} dgx_dsi_late_t;

static void dgx_dsi_late_frame_end(dgx_dsi_bus_t *bus, uint32_t now);
static void dgx_dsi_late_now(dgx_dsi_bus_t *bus);
static void dgx_dsi_late_idle(dgx_dsi_bus_t *bus);
static void dgx_dsi_late_shares(dgx_dsi_bus_t *bus);
static void dgx_dsi_late_timed(dgx_dsi_late_t *late, uint32_t bytes, uint32_t us);
static void dgx_dsi_late_back(dgx_dsi_bus_t *bus);
static void dgx_dsi_late_moved(dgx_dsi_bus_t *bus);
static void dgx_dsi_late_clear(dgx_dsi_bus_t *bus, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
#endif

struct _dgx_dsi_bus_t {
    dgx_bus_protocols_t       protocols;
    esp_lcd_dsi_bus_handle_t  dsi;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
    esp_ldo_channel_handle_t  phy_power;
    dgx_fb_area_t             area; /* area.fb is NULL until dgx_dsi_video_setup(); the frame buffer drawing goes into */
    uint8_t                   data_lanes;
    /*
     * With two frame buffers drawing goes into the one the display is not
     * reading. A batch of drawing ends with the display told to read that
     * one from the next frame on; when it does, the other one is free and
     * becomes the one drawing goes into. Until it does, batches of other
     * tasks go into the same picture: `flip` keeps that. The frame buffer
     * that becomes free lacks what was drawn since it was last drawn into:
     * `tiles` keeps where that is, and it is taken from the frame buffer
     * the display reads tile by tile, when a tile is drawn in or read, and
     * the rest when the picture is finished: small pieces at once, large
     * ones after the display has taken the picture, see `late`.
     */
    uint8_t          *frame[2];
    dgx_fb_tiles_t    tiles;        /* the maps are there with two frame buffers only */
    dgx_fb_flip_t     flip;         /* which of them drawing goes into, and whether the display was told to read it */
#if DGX_DSI_HARDWARE_COPY
    /* the 2D DMA: a place among those who use it, two descriptors of a picture one after another, a transfer, and the word that it is done */
    dma2d_pool_handle_t dma_pool; /* NULL when there is none: the processor copies then */
    dma2d_descriptor_t *dma_desc;
    dma2d_trans_t      *dma_trans;
    SemaphoreHandle_t   dma_done;
    bool                dma_exchange; /* the transfer under way exchanges the bytes of 16-bit pixels */
#endif
#if DGX_DSI_HARDWARE_FILL
    ppa_client_handle_t filler; /* the accelerator that fills rectangles; NULL when there is none: the processor fills then */
#if DGX_DSI_HARDWARE_MIX
    ppa_client_handle_t mixer;  /* the same accelerator mixing two colors by an amount of ink; NULL when there is none */
#endif
#endif
    bool              rows_written; /* pixels were put into the frame buffer past the processor: there is something to show */
    bool              turned;       /* the screen does not lie on the panel pixel for pixel: mirrored, or its axes swapped */
    bool              window_open;  /* pixels are being sent into the window from its first one on: its tiles wait to be told how far they went */
    uint32_t          window_got;   /* how many came */
    uint8_t           frames;       /* 1 or 2 */
    uint8_t           tile_shift_x; /* the tiles of two frame buffers are 1 << this pixels wide, or wider when the panel makes them so */
    uint8_t           tile_shift_y; /* and 1 << this rows high */
    bool              started;      /* the display is being sent the frame buffer */
    bool              after_scan;   /* one frame buffer, a batch brought out behind the scan of the display */
    bool              batch_seen;   /* a part of the batch being drawn is on the display already: holding the rest back would split it */
    uint16_t          lines_total;  /* rows of a frame with the blanking */
    uint16_t          lines_before; /* rows of blanking between the end of a frame, where it is counted, and its first row */
    SemaphoreHandle_t refreshed;
    volatile uint32_t refreshes;         /* frames the display has been sent, counted where a frame ends */
    volatile uint32_t refresh_us;        /* when the last one ended, the low 32 bits of the time: read whole from a task */
    volatile uint32_t refresh_period_us; /* and how long it took */
    volatile uint32_t frame_us_256;      /* how long a frame takes, in 1/256 us: by the timings of the panel, then by the frames counted */
    uint32_t          frames_from;       /* the count of frames and the time where the frames now being timed start; 0 when none yet */
    uint32_t          frames_from_us;
#if DGX_DSI_HARDWARE_COPY
    dgx_dsi_late_t *late; /* what a finished picture lacks and is given after the display has taken it; NULL when nothing is left for later */
#endif
    dgx_dsi_tuning_t tuning; /* the numbers the bus decides by: those of the board it was written on, until a program gives its own */
    /* counted since the start, for dgx_dsi_counters() */
    uint32_t batches;      /* batches of drawing that ended with something drawn */
    uint32_t scan_waits;   /* behind the scan: times a write waited for the scan */
    uint32_t scan_wait_us; /* and how long in all */
    /*
     * A DSI packet carries a command together with its parameters, so a
     * command is held back until its data comes, or until it is known that
     * there is none: another command, or sync_write().
     */
    uint8_t command;
    bool    command_pending;
};

static const char TAG[] = "DGX DSI BUS";

static void dgx_dsi_send_pending(dgx_dsi_bus_t *bus)
{
    if (!bus->command_pending) return;
    bus->command_pending = false;
    esp_err_t ret        = esp_lcd_panel_io_tx_param(bus->io, bus->command, NULL, 0);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x failed: %s", bus->command, esp_err_to_name(ret));
}

/*
 * The tiles the panel is cut into to tell what one of two frame buffers
 * lacks, unless dgx_dsi_tiles() gives another size: see the header for the
 * size and docs/dsi-display-en.md for what it was chosen by. A row of tiles
 * is a word of each of the two maps.
 */
#define DGX_DSI_TILE_SHIFT_X __builtin_ctz(DGX_DSI_TILE_WIDTH)
#define DGX_DSI_TILE_SHIFT_Y __builtin_ctz(DGX_DSI_TILE_HEIGHT)

/*
 * The numbers below are what a bus starts with, see dgx_dsi_tuning_t: those
 * of the board it was written on. dgx_dsi_bench() measures them on another.
 */

/*
 * How close to the end of a frame a word to the display is not relied on:
 * told to change the frame buffer it may not do it at once, told to stay
 * with the one it reads it may have changed already.
 */
#define DGX_DSI_FLIP_GUARD_US 2000

/*
 * A rectangle goes to the 2D DMA when it is large enough. Measured on an
 * ESP32-P4 with the display running: the DMA takes 7 us a KB and about
 * 0.2 ms to be asked, with the cache of the rows it writes taken out and
 * the wait for it; the processor 1.6 us a row and 1.4 us for every 32
 * pixels of it. A rectangle of 64x48 is still faster by the processor, one
 * of 320x100 or a bar of 40x548 twice as fast by the DMA.
 */
#define DGX_DSI_HARDWARE_COPY_FROM 8000u
/* and in that count a row costs the processor as much as this many pixels of it: rows * (width + this) is compared */
#define DGX_DSI_HARDWARE_COPY_ROW_PX 53u

/*
 * A rectangle is filled by the accelerator when rows + bytes / 64 is this
 * or more. Measured on an ESP32-P4: the accelerator takes 57 us to be asked,
 * 0.2 us a row and 3 us a KB, the processor 0.9 us a row and 14 us a KB.
 * They meet at about 80; more is taken, for what the accelerator has filled
 * is not in the cache, and what is drawn over it next has to fetch it.
 * A square of 48x48 stays with the processor, one of 100x100 and a strip of
 * 4x548 go to the accelerator.
 */
#define DGX_DSI_HARDWARE_FILL_FROM 200u
#define DGX_DSI_HARDWARE_FILL_DIV 64u
/* no rectangle the accelerator takes has less than this in its width and height together: a cheap test for the short runs of figures and letters */
#define DGX_DSI_HARDWARE_FILL_SUM 64u

/*
 * Rows of pixels from a program go to the 2D DMA from this many bytes on:
 * through the window the processor moves about 36 KB in a millisecond, so
 * less than this it has moved before the DMA is asked.
 */
#define DGX_DSI_HARDWARE_ROWS_FROM 16384u

#if DGX_DSI_HARDWARE_MIX
static bool dgx_dsi_write_ink(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *ink,
                              uint32_t pitch, uint32_t paper, uint32_t color);
#endif

/* a frame has been sent: the display starts the next one from the frame buffer it was last told to read */
static IRAM_ATTR bool dgx_dsi_on_refresh(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)user_ctx;
    uint32_t       now = (uint32_t)esp_timer_get_time();
    BaseType_t     woken = pdFALSE;
#if DGX_DSI_HARDWARE_COPY
    /*
     * The display has taken the frame buffer it was told: what the picture
     * lacks goes into it ahead of its scan. Before the frame is counted: a
     * task that sees the count finds the pieces taken.
     */
    if (bus->late) dgx_dsi_late_frame_end(bus, now);
#endif
    /* the count first: a task that reads the time and then the count, and finds the count as it was, has the time of that frame */
    const uint32_t count = ++bus->refreshes;
    bus->refresh_period_us = now - bus->refresh_us;
    /*
     * How long a frame takes: many of them timed together, so that a late
     * interrupt hardly counts. 16 frames at first, to have a number soon,
     * then 256 at a time.
     */
    if (!(count & (count < 256u ? 15u : 255u))) {
        const uint32_t frames = count - bus->frames_from, took = now - bus->frames_from_us, nominal = bus->frame_us_256;
        if (bus->frames_from && frames <= 256u && took < (1u << 24)) {
            const uint32_t frame = took * 256u / frames;
            /* what is far from the timings of the panel is not a frame: the display was stopped meanwhile, or the clock is not what was asked */
            if (frame - (nominal >> 1) < nominal) bus->frame_us_256 = frame;
        }
        bus->frames_from    = count;
        bus->frames_from_us = now;
    }
    bus->refresh_us        = now;
    xSemaphoreGiveFromISR(bus->refreshed, &woken);
    return woken == pdTRUE;
}

static void dgx_dsi_bring_out(uint8_t *from, size_t length)
{
    esp_cache_msync(from, length, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/*
 * What the two frame buffers that change places are told of the display:
 * see dgx_fb_flip_priv.h.
 */

static void dgx_dsi_flip_show(void *context, uint8_t frame)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)context;
    /* a frame buffer of its own given to the panel is not copied: the panel reads it from the next frame on */
    esp_lcd_panel_draw_bitmap(bus->panel, 0, 0, bus->area.fb_width, 1, bus->frame[frame]);
}

static bool dgx_dsi_flip_frames(void *context, uint32_t *ended, uint32_t *since_us, uint32_t *period_us)
{
    const dgx_dsi_bus_t *bus = (const dgx_dsi_bus_t *)context;
    /* the time before the count, the other way round from where they are set */
    const uint32_t at = bus->refresh_us;
    *ended            = bus->refreshes;
    *period_us        = bus->refresh_period_us;
    *since_us         = (uint32_t)esp_timer_get_time() - at;
    return *period_us >= 1000u && *period_us <= 100000u && *since_us <= 2u * *period_us;
}

static bool dgx_dsi_flip_wait(void *context)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)context;
#if DGX_DSI_HARDWARE_COPY
    /* the task has to wait for the display anyway: what the picture lacks is copied meanwhile, when there is time for it */
    dgx_dsi_late_now(bus);
#endif
    return xSemaphoreTake(bus->refreshed, pdMS_TO_TICKS(200)) == pdTRUE;
}

static uint32_t dgx_dsi_flip_now(void *context)
{
    return (uint32_t)esp_timer_get_time();
}

static const dgx_fb_flip_display_t dgx_dsi_flip_display = {
    .show   = dgx_dsi_flip_show,
    .frames = dgx_dsi_flip_frames,
    .wait   = dgx_dsi_flip_wait,
    .now_us = dgx_dsi_flip_now,
};

/*
 * The display was told to read the frame buffer drawing went into, and
 * something is about to be drawn. The batch goes into the same picture when
 * the display has not taken it and the frame does not end soon; otherwise
 * this waits until the display reads it and drawing goes into the other
 * frame buffer, which lacks what was drawn since it was last drawn into.
 */
static void dgx_dsi_draw_moved(dgx_dsi_bus_t *bus)
{
    if (!dgx_fb_flip_draw(&bus->flip)) {
#if DGX_DSI_HARDWARE_COPY
        dgx_dsi_late_back(bus);
#endif
        return;
    }
#if DGX_DSI_HARDWARE_COPY
    dgx_dsi_late_moved(bus);
#endif
    bus->area.fb = bus->frame[bus->flip.drawn];
    dgx_fb_tiles_changed(&bus->tiles);
}

/* before anything is drawn */
static inline void dgx_dsi_draw_ready(dgx_dsi_bus_t *bus)
{
    if (bus->flip.told) dgx_dsi_draw_moved(bus);
}

/* the frame buffer the display reads, or is about to: it has what the one drawn into lacks */
static inline const uint8_t *dgx_dsi_other(const dgx_dsi_bus_t *bus)
{
    return bus->frame[bus->flip.drawn ^ 1u];
}

/*
 * Pixels were being sent into the window, and now something else comes. If
 * they did not fill it, what they did not reach is as old as the frame
 * buffer was: it is taken from the other one. The pixels came row after row
 * of the screen, so that is the rest of a row and the rows after it.
 */
static void dgx_dsi_window_close(dgx_dsi_bus_t *bus)
{
    if (!bus->window_open) return;
    bus->window_open = false;
    const dgx_fb_area_t *area  = &bus->area;
    const uint32_t       width = (uint32_t)area->right - area->left + 1u, height = (uint32_t)area->bottom - area->top + 1u;
    uint16_t             x0, y0, x1, y1, px0, py0, px1, py1;
    dgx_fb_area_to_panel(area, area->left, area->right, area->top, area->bottom, &x0, &y0, &x1, &y1);
    uint32_t rows = bus->window_got / width, rest = bus->window_got % width;
    if (rows < height) {
        if (rest) {
            dgx_fb_area_to_panel(area, (int)(area->left + rest), area->right, (int)(area->top + rows), (int)(area->top + rows), &px0, &py0, &px1, &py1);
            dgx_fb_tiles_unwritten(&bus->tiles, area->fb, dgx_dsi_other(bus), x0, y0, x1, y1, px0, py0, px1, py1);
            ++rows;
        }
        if (rows < height) {
            dgx_fb_area_to_panel(area, area->left, area->right, (int)(area->top + rows), area->bottom, &px0, &py0, &px1, &py1);
            dgx_fb_tiles_unwritten(&bus->tiles, area->fb, dgx_dsi_other(bus), x0, y0, x1, y1, px0, py0, px1, py1);
        }
    }
    dgx_fb_tiles_close(&bus->tiles, x0, y0, x1, y1);
}

/*
 * With two frame buffers, before a rectangle of the screen is written or
 * read: the frame buffer drawing goes into gets what it lacks there.
 * `whole` tells that every pixel of the rectangle is about to be written.
 */
static void dgx_dsi_tiles_ready(dgx_dsi_bus_t *bus, int left, int right, int top, int bottom, bool whole, bool drawn)
{
    uint16_t x0, y0, x1, y1;
    dgx_dsi_window_close(bus);
    dgx_fb_area_to_panel(&bus->area, left, right, top, bottom, &x0, &y0, &x1, &y1);
    dgx_fb_tiles_ready(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), x0, y0, x1, y1, whole, drawn);
}

/* a look at a finished picture has copied tiles into it: no batch is there to bring them out of the cache */
static inline void dgx_dsi_read_out(dgx_dsi_bus_t *bus)
{
    uint32_t from, length;
    if (dgx_fb_tiles_take_copied(&bus->tiles, &from, &length)) dgx_dsi_bring_out(bus->area.fb + from, length);
}

#if DGX_DSI_HARDWARE_COPY
/* the 2D DMA has finished a transfer */
static IRAM_ATTR bool dgx_dsi_dma_done(dma2d_channel_handle_t channel, dma2d_event_data_t *event, void *user)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(((dgx_dsi_bus_t *)user)->dma_done, &woken);
    return woken == pdTRUE;
}

/* the 2D DMA has channels for the transfer: set them up and start it */
static IRAM_ATTR bool dgx_dsi_dma_picked(uint32_t count, const dma2d_trans_channel_info_t *channels, void *user)
{
    dgx_dsi_bus_t         *bus = (dgx_dsi_bus_t *)user;
    dma2d_channel_handle_t tx = NULL, rx = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_TX) tx = channels[i].chan;
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_RX) rx = channels[i].chan;
    }
    dma2d_trigger_t memory = {.periph = DMA2D_TRIG_PERIPH_M2M, .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX};
    dma2d_connect(tx, &memory);
    memory.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    dma2d_connect(rx, &memory);
    /*
     * The channels are left with the bursts they are given when connected,
     * 128 bytes: the structure that sets them is not the same in every IDF.
     */
    if (bus->dma_exchange) {
        /* of the three bytes the DMA may put in another order a 16-bit pixel has the lower two: they change places */
        dma2d_csc_config_t exchange = {
            .tx_csc_option = DMA2D_CSC_TX_SCRAMBLE,
            .pre_scramble  = DMA2D_SCRAMBLE_ORDER_BYTE2_0_1,
        };
        dma2d_configure_color_space_conversion(tx, &exchange);
    }
    dma2d_rx_event_callbacks_t callbacks = {.on_recv_eof = dgx_dsi_dma_done};
    dma2d_register_rx_event_callbacks(rx, &callbacks, bus);
    dma2d_set_desc_addr(tx, (intptr_t)&bus->dma_desc[0]);
    dma2d_set_desc_addr(rx, (intptr_t)&bus->dma_desc[1]);
    if (bus->dma_exchange) {
        /*
         * The chip has two ways to take pixels whose bytes it puts in
         * another order: 2, "other 2 byte/pixel type", and 3, the same for
         * 3 bytes. IDF 5.5.4 sets 2, IDF 6.1 sets 3 and says that only
         * pixels of 3 bytes are supported; with 2-byte pixels the transfer
         * never ends then (esp-idf issue 19226). So the field is set here,
         * in the channel that was given the descriptor: the driver does not
         * tell its number.
         */
        for (size_t i = 0; i < sizeof(DMA2D.out_channel) / sizeof(DMA2D.out_channel[0]); ++i) {
            if (DMA2D.out_channel[i].out_link_addr.outlink_addr_chn == (uint32_t)(intptr_t)&bus->dma_desc[0]) {
                DMA2D.out_channel[i].out_color_convert.out_color_input_sel_chn = 2;
            }
        }
    }
    dma2d_start(tx);
    dma2d_start(rx);
    return false;
}

/* a picture that is `rows` rows of `stride` pixels from `first` on, and the rectangle `width` wide from its column `x` */
static void dgx_dsi_dma_describe(dma2d_descriptor_t *desc, const uint8_t *first, uint32_t stride, uint32_t x, uint32_t width, uint32_t rows, uint8_t pixel_bytes)
{
    memset(desc, 0, sizeof(*desc));
    desc->buffer    = (void *)first;
    desc->dma2d_en  = 1;
    desc->suc_eof   = 1;
    desc->ha_length = stride;
    desc->va_size   = rows;
    desc->hb_length = width;
    desc->vb_size   = rows;
    desc->x         = x;
    desc->pbyte     = pixel_bytes == 2 ? DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL : DMA2D_DESCRIPTOR_PBYTE_3B0_PER_PIXEL;
    desc->mode      = DMA2D_DESCRIPTOR_BLOCK_RW_MODE_SINGLE;
    desc->owner     = DMA2D_DESCRIPTOR_BUFFER_OWNER_DMA;
}

/*
 * Move a rectangle of `width` by `rows` pixels by the 2D DMA and wait until
 * it is there. `from` and `to` are the first rows of the rectangle in their
 * pictures, rows of `from_stride` pixels and of the width of the panel, and
 * the rectangle starts at the columns `from_x` and `to_x` of them.
 *
 * The DMA reads and writes memory itself, so what the cache holds of the
 * source is brought out first, and what it holds of the place written is
 * brought out and forgotten: what is read there afterwards comes from
 * memory.
 *
 * `behind_scan` makes it start when the display is not reading the rows:
 * the DMA writes faster than the display reads and stays ahead of it.
 */
static bool dgx_dsi_dma(dgx_dsi_bus_t *bus, const uint8_t *from, uint32_t from_stride, uint32_t from_x, uint8_t *to, uint32_t to_x, uint32_t width,
                        uint32_t rows, bool exchange, bool behind_scan)
{
    const uint32_t bytes   = bus->area.pixel_bytes;
    const size_t   to_pitch = (size_t)bus->area.fb_width * bytes;
    /* pieces that are only just ahead of the scan of the display are not slowed down by another transfer */
    dgx_dsi_late_shares(bus);
    esp_cache_msync((void *)(from + (size_t)from_x * bytes), (size_t)(rows - 1u) * from_stride * bytes + (size_t)width * bytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(to + (size_t)to_x * bytes, (size_t)(rows - 1u) * to_pitch + (size_t)width * bytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    dgx_dsi_dma_describe(&bus->dma_desc[0], from, from_stride, from_x, width, rows, (uint8_t)bytes);
    dgx_dsi_dma_describe(&bus->dma_desc[1], to, bus->area.fb_width, to_x, width, rows, (uint8_t)bytes);
    esp_cache_msync(bus->dma_desc, 2u * sizeof(dma2d_descriptor_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    bus->dma_exchange = exchange;
    if (behind_scan) {
        const uint32_t first = (uint32_t)((size_t)(to - bus->area.fb) / to_pitch);
        dgx_dsi_wait_scan_outside(bus, first, first + rows - 1u);
    }
    static const dma2d_trans_config_t transfer = {.tx_channel_num = 1, .rx_channel_num = 1, .on_job_picked = dgx_dsi_dma_picked};
    dma2d_trans_config_t              config   = transfer;
    config.channel_flags = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING | (exchange ? DMA2D_CHANNEL_FUNCTION_FLAG_TX_CSC : 0u);
    config.user_config   = bus;
    xSemaphoreTake(bus->dma_done, 0); /* nothing is left over from a transfer that was given up */
    const bool     undisturbed = bus->late && !bus->late->left;
    const uint32_t started     = (uint32_t)esp_timer_get_time();
    if (dma2d_enqueue(bus->dma_pool, &config, bus->dma_trans) != ESP_OK) return false;
    xSemaphoreTake(bus->dma_done, portMAX_DELAY);
    /* how fast the 2D DMA is here is what tells whether a piece may be left for later */
    if (undisturbed && !bus->late->left) dgx_dsi_late_timed(bus->late, width * rows * bytes, (uint32_t)esp_timer_get_time() - started);
    return true;
}

/* what is left to copy from one frame buffer into the other before they change places */
static bool dgx_dsi_hardware_copy(void *context, uint8_t *to, const uint8_t *from, uint32_t left, uint32_t top, uint32_t width, uint32_t rows)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)context;
    if (rows * (width + bus->tuning.copy_row_px) < bus->tuning.copy_from) return false;
    const size_t pitch = (size_t)bus->area.fb_width * bus->area.pixel_bytes;
    return dgx_dsi_dma(bus, from + (size_t)top * pitch, bus->area.fb_width, left, to + (size_t)top * pitch, left, width, rows, false, false);
}

static void dgx_dsi_dma_free(dgx_dsi_bus_t *bus)
{
    if (bus->dma_pool) dma2d_release_pool(bus->dma_pool);
    if (bus->dma_done) vSemaphoreDelete(bus->dma_done);
    free(bus->dma_trans);
    free(bus->dma_desc);
    bus->dma_pool  = NULL;
    bus->dma_done  = NULL;
    bus->dma_trans = NULL;
    bus->dma_desc  = NULL;
}

/* take a place among the users of the 2D DMA; without it the processor does the work */
static void dgx_dsi_dma_init(dgx_dsi_bus_t *bus)
{
    const dma2d_pool_config_t pool_config = {0};
    /* the descriptors are read by the DMA: memory it can reach, on a boundary of a line of the cache */
    bus->dma_desc  = (dma2d_descriptor_t *)heap_caps_aligned_calloc(64, 1, 64, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bus->dma_trans = (dma2d_trans_t *)heap_caps_calloc(1, dma2d_get_trans_elm_size(), MALLOC_CAP_INTERNAL);
    bus->dma_done  = xSemaphoreCreateBinary();
    if (!bus->dma_desc || !bus->dma_trans || !bus->dma_done || dma2d_acquire_pool(&pool_config, &bus->dma_pool) != ESP_OK) {
        ESP_LOGW(TAG, "no 2D DMA: the processor copies");
        bus->dma_pool = NULL;
        dgx_dsi_dma_free(bus);
    }
}
#endif

/*
 * Measured on an ESP32-P4: what the cache brings out in a millisecond and
 * what a window takes in from a buffer in one, in KB, with room; and how far
 * ahead of the row it shows the display reads memory (it takes the first
 * rows of a frame as soon as the frame before has ended).
 */
#define DGX_DSI_OUT_KB_A_MS  256u
#define DGX_DSI_IN_KB_A_MS   28u
#define DGX_DSI_SCAN_LEAD    6u
#define DGX_DSI_SCAN_ROOM_US 150u
/* how long it may take from the moment it is asked to the first byte the 2D DMA or the accelerator writes */
#define DGX_DSI_DMA_START_US 400u

/*
 * Wait, if needed, for the moment the rows `first`..`last` of the frame
 * buffer can be changed for `takes` microseconds without the display
 * reading them meanwhile. The display reads the frame buffer row after row,
 * a frame in the time between two ends of a frame; where it is now follows
 * from the time since the last one. The best moment is when it has just left
 * the rows: then it is a whole frame away from them.
 */
static bool dgx_dsi_wait_scan(dgx_dsi_bus_t *bus, uint32_t first, uint32_t last, uint32_t takes)
{
    uint32_t period = bus->refresh_period_us;
    uint32_t since  = (uint32_t)esp_timer_get_time() - bus->refresh_us;
    if (period < 1000u || period > 100000u || since > 2u * period) return false; /* no frames counted: nothing to go by */
    if (since >= period) since -= period;                                  /* the end of a frame is being counted right now */
    const uint64_t line   = ((uint64_t)period << 16) / bus->lines_total; /* a row in 1/65536 us */
    const uint32_t before = bus->lines_before + first;
    /* the scan is in the rows from `enter` to `leave`, counted from the end of the last frame */
    const uint32_t enter = first < bus->tuning.scan_lead ? 0u : (uint32_t)(((before - bus->tuning.scan_lead) * line) >> 16);
    const uint32_t leave = (uint32_t)(((bus->lines_before + last + 1u) * line) >> 16);
    takes += bus->tuning.scan_room_us;
    if (takes >= period - (leave - enter)) return false;               /* too much for any moment */
    if (since + takes < enter) return true;                            /* done before the scan comes */
    if (since >= leave && since + takes < enter + period) return true; /* the scan has left and is not back in time */
    /* the scan is in the rows, or comes too soon: when it has left them, in this frame or in the next */
    uint32_t until = since < leave ? leave : leave + period;
    uint32_t wait  = until - since;
    uint32_t start = bus->refresh_us + since;
    ++bus->scan_waits;
    bus->scan_wait_us += wait;
    if (wait > 2u * portTICK_PERIOD_MS * 1000u) vTaskDelay(wait / (portTICK_PERIOD_MS * 1000u) - 1u);
    while ((uint32_t)esp_timer_get_time() - start < wait) {
    }
    return true;
}

/* the rows of the frame buffer `length` bytes from `from` on lie in */
static void dgx_dsi_wait_scan_bytes(dgx_dsi_bus_t *bus, uint32_t from, uint32_t length)
{
    const uint32_t pitch = (uint32_t)bus->area.fb_width * bus->area.pixel_bytes;
    /* what is still in the cache is what takes time to bring out */
    uint32_t kept = length;
#ifdef CONFIG_CACHE_L2_CACHE_SIZE
    if (kept > (uint32_t)CONFIG_CACHE_L2_CACHE_SIZE) kept = (uint32_t)CONFIG_CACHE_L2_CACHE_SIZE;
#endif
    dgx_dsi_wait_scan(bus, from / pitch, (from + length - 1u) / pitch, kept / bus->tuning.out_kb_a_ms * 1000u / 1024u);
}

/*
 * Pixels are about to be sent for the whole window: wait until the scan has
 * left its rows. The cache keeps only so much, and what it lets out during
 * the transfer is seen at once; sent behind the scan, it is seen whole in
 * the next frame all the same. A window that cannot be sent before the scan
 * is back goes as it comes, and the batch with it.
 */
static void dgx_dsi_wait_scan_window(dgx_dsi_bus_t *bus)
{
    uint16_t x0, y0, x1, y1;
    dgx_fb_area_to_panel(&bus->area, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, &x0, &y0, &x1, &y1);
    uint32_t bytes = (uint32_t)(x1 - x0 + 1u) * (uint32_t)(y1 - y0 + 1u) * bus->area.pixel_bytes;
    if (!dgx_dsi_wait_scan(bus, y0, y1, bytes / bus->tuning.window_kb_a_ms * 1000u / 1024u)) bus->batch_seen = true;
}

#if DGX_DSI_HARDWARE_COPY || DGX_DSI_HARDWARE_FILL

/*
 * Wait, if needed, until the display is not reading the rows `first`..`last`
 * and will not start to in the next moments. What is written from then on
 * faster than the display reads stays ahead of its scan: the rows are seen
 * whole in the frame that follows.
 */
static bool dgx_dsi_wait_scan_outside(dgx_dsi_bus_t *bus, uint32_t first, uint32_t last)
{
    uint32_t period = bus->refresh_period_us;
    uint32_t since  = (uint32_t)esp_timer_get_time() - bus->refresh_us;
    if (period < 1000u || period > 100000u || since > 2u * period) return false; /* no frames counted: nothing to go by */
    if (since >= period) since -= period;
    const uint64_t line   = ((uint64_t)period << 16) / bus->lines_total;
    const uint32_t before = bus->lines_before + first;
    uint32_t       enter  = first < bus->tuning.scan_lead ? 0u : (uint32_t)(((before - bus->tuning.scan_lead) * line) >> 16);
    const uint32_t leave  = (uint32_t)(((bus->lines_before + last + 1u) * line) >> 16);
    enter = enter > bus->tuning.dma_start_us ? enter - bus->tuning.dma_start_us : 0u;
    if (since < enter || since >= leave) return true;
    uint32_t wait  = leave - since;
    uint32_t start = bus->refresh_us + since;
    ++bus->scan_waits;
    bus->scan_wait_us += wait;
    if (wait > 2u * portTICK_PERIOD_MS * 1000u) vTaskDelay(wait / (portTICK_PERIOD_MS * 1000u) - 1u);
    while ((uint32_t)esp_timer_get_time() - start < wait) {
    }
    return true;
}
#endif

#if DGX_DSI_HARDWARE_COPY
/*
 * What a finished picture lacks and is given after the display has taken
 * it. See the note where dgx_dsi_late_piece_t is declared.
 */

static inline dma2d_trans_t *dgx_dsi_late_trans(const dgx_dsi_bus_t *bus, uint32_t piece)
{
    return (dma2d_trans_t *)(bus->late->trans + piece * bus->late->trans_size);
}

static IRAM_ATTR void dgx_dsi_late_start(dgx_dsi_bus_t *bus, uint32_t piece)
{
    /* a piece the DMA does not take stays marked as not copied: the task that waits for it copies it itself */
    dma2d_enqueue(bus->dma_pool, &bus->late->config[piece], dgx_dsi_late_trans(bus, piece));
}

/*
 * A copy of `bytes` by the 2D DMA took `us` microseconds with no other
 * transfer or fill going on: the slowest of the last ones is kept,
 * forgotten by an eighth a copy. A small one tells more of the starting
 * than of the copying and is not counted.
 */
static IRAM_ATTR void dgx_dsi_late_timed(dgx_dsi_late_t *late, uint32_t bytes, uint32_t us)
{
    if (bytes < DGX_DSI_LATE_TIMED_FROM) return;
    const uint32_t took = (uint32_t)(((uint64_t)us << 20) / bytes), less = late->mb_us - (late->mb_us >> 3);
    late->mb_us         = took > less ? took : less;
}

/* in the interrupt of the 2D DMA: a piece has been copied */
static IRAM_ATTR bool dgx_dsi_late_done(dma2d_channel_handle_t channel, dma2d_event_data_t *event, void *user)
{
    dgx_dsi_bus_t *bus   = ((dgx_dsi_late_context_t *)user)->bus;
    const uint32_t piece = ((dgx_dsi_late_context_t *)user)->piece;
    const dgx_dsi_late_piece_t *part  = &bus->late->piece[piece];
    const uint32_t              now   = (uint32_t)esp_timer_get_time();
    const uint32_t              bytes = (uint32_t)part->width * part->rows * bus->area.pixel_bytes;
    bus->late->bytes += bytes;
    if (!bus->late->shared) dgx_dsi_late_timed(bus->late, bytes, now - bus->late->piece_us);
    /* where the display reads now, by the time since the frame ended and the rows it reads ahead: has it come past the piece before the copy was there? */
    const uint32_t period = bus->refresh_period_us, since = now - bus->late->from_us;
    if (period >= 1000u && (uint64_t)since * bus->lines_total >= (uint64_t)(bus->lines_before + part->top + part->rows - bus->tuning.scan_lead) * period) {
        ++bus->late->behind;
    }
    bus->late->left = (uint8_t)(bus->late->left & ~(1u << piece));
    if (piece + 1u < bus->late->count) dgx_dsi_late_start(bus, piece + 1u);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(bus->late->step, &woken);
    return woken == pdTRUE;
}

/* the 2D DMA has channels for a piece: set them up and start */
static IRAM_ATTR bool dgx_dsi_late_picked(uint32_t count, const dma2d_trans_channel_info_t *channels, void *user)
{
    dgx_dsi_bus_t         *bus   = ((dgx_dsi_late_context_t *)user)->bus;
    const uint32_t         piece = ((dgx_dsi_late_context_t *)user)->piece;
    dma2d_channel_handle_t tx = NULL, rx = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_TX) tx = channels[i].chan;
        if (channels[i].dir == DMA2D_CHANNEL_DIRECTION_RX) rx = channels[i].chan;
    }
    dma2d_trigger_t memory = {.periph = DMA2D_TRIG_PERIPH_M2M, .periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_TX};
    dma2d_connect(tx, &memory);
    memory.periph_sel_id = SOC_DMA2D_TRIG_PERIPH_M2M_RX;
    dma2d_connect(rx, &memory);
    dma2d_rx_event_callbacks_t callbacks = {.on_recv_eof = dgx_dsi_late_done};
    dma2d_register_rx_event_callbacks(rx, &callbacks, user);
    dma2d_set_desc_addr(tx, (intptr_t)&bus->late->desc[2u * piece]);
    dma2d_set_desc_addr(rx, (intptr_t)&bus->late->desc[2u * piece + 1u]);
    bus->late->piece_us = (uint32_t)esp_timer_get_time();
    dma2d_start(tx);
    dma2d_start(rx);
    return false;
}

/* in the interrupt where a frame ends */
static IRAM_ATTR void dgx_dsi_late_frame_end(dgx_dsi_bus_t *bus, uint32_t now)
{
    if (!atomic_exchange(&bus->late->armed, 0u)) return;
    bus->late->from_us = now;
    dgx_dsi_late_start(bus, 0);
}

/* copy a piece here and now, from one frame buffer into the other */
static void dgx_dsi_late_copy(dgx_dsi_bus_t *bus, const uint8_t *from, uint8_t *to, const dgx_dsi_late_piece_t *part)
{
    const uint32_t bytes = bus->area.pixel_bytes;
    const size_t   pitch = (size_t)bus->area.fb_width * bytes;
    if (dgx_dsi_dma(bus, from + (size_t)part->top * pitch, bus->area.fb_width, part->left, to + (size_t)part->top * pitch, part->left, part->width, part->rows,
                    false, false)) {
        return;
    }
    for (uint32_t y = part->top; y < (uint32_t)part->top + part->rows; ++y) {
        memcpy(to + (size_t)y * pitch + (size_t)part->left * bytes, from + (size_t)y * pitch + (size_t)part->left * bytes, (size_t)part->width * bytes);
    }
    dgx_dsi_bring_out(to + (size_t)part->top * pitch + (size_t)part->left * bytes, (size_t)(part->rows - 1u) * pitch + (size_t)part->width * bytes);
}

/* copy the pieces of `mask` here and now, from the frame buffer they were to come from */
static void dgx_dsi_late_by_hand(dgx_dsi_bus_t *bus, uint32_t mask)
{
    for (uint32_t i = 0; i < bus->late->count; ++i) {
        if (mask & (1u << i)) dgx_dsi_late_copy(bus, bus->frame[bus->late->into ^ 1u], bus->frame[bus->late->into], &bus->late->piece[i]);
    }
}

/*
 * Wait until a piece more has been copied than when `left` was read; false
 * when none is for a time many frames long.
 */
static bool dgx_dsi_late_step(dgx_dsi_bus_t *bus, uint32_t left)
{
    for (int tries = 0; tries < 50; ++tries) {
        if (bus->late->left != left) return true;
        xSemaphoreTake(bus->late->step, pdMS_TO_TICKS(4) + 1);
    }
    if (bus->late->left != left) return true;
    /* the 2D DMA did not take a piece, or its interrupt never came: what is left is copied here */
    ESP_LOGW(TAG, "a piece left for the 2D DMA was not copied: the task copies");
    left           = bus->late->left;
    bus->late->left = 0;
    dgx_dsi_late_by_hand(bus, left);
    return false;
}

/* wait until nothing is being copied behind the back of the tasks */
static void dgx_dsi_late_idle(dgx_dsi_bus_t *bus)
{
    if (!bus->late || !bus->late->left) return;
    const uint32_t from = (uint32_t)esp_timer_get_time();
    ++bus->late->waits;
    for (uint32_t left = bus->late->left; left && dgx_dsi_late_step(bus, left); left = bus->late->left) {
    }
    const uint32_t waited = (uint32_t)esp_timer_get_time() - from;
    bus->late->wait_us += waited;
    /* only the first batch of a picture waits so: a batch that joins it comes when the pieces are long there */
    dgx_fb_flip_idled(&bus->flip, waited);
}

/*
 * Before a rectangle of the panel is written, both ends included. A piece
 * that is not copied yet is read from the frame buffer drawing goes into
 * now: writing over it has to wait until it has gone.
 */
static void dgx_dsi_late_clear(dgx_dsi_bus_t *bus, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint32_t from   = 0;
    bool     waited = false;
    for (;;) {
        const uint32_t left  = bus->late->left;
        bool           meets = false;
        for (uint32_t i = 0; i < bus->late->count && !meets; ++i) {
            const dgx_dsi_late_piece_t *part = &bus->late->piece[i];
            meets = (left & (1u << i)) && x0 < (uint32_t)part->left + part->width && x1 >= part->left && y0 < (uint32_t)part->top + part->rows && y1 >= part->top;
        }
        if (!meets) break;
        if (!waited) {
            waited = true;
            from   = (uint32_t)esp_timer_get_time();
            ++bus->late->waits;
        }
        dgx_dsi_late_step(bus, left);
    }
    if (waited) {
        const uint32_t took = (uint32_t)esp_timer_get_time() - from;
        bus->late->wait_us += took;
        dgx_fb_flip_idled(&bus->flip, took);
    }
}

/*
 * A batch goes into the picture that was finished, and the display has been
 * told to stay with the frame buffer it reads: the pieces are not to start,
 * the batch may draw over them.
 */
static void dgx_dsi_late_back(dgx_dsi_bus_t *bus)
{
    if (!bus->late || !bus->late->count) return;
    if (atomic_exchange(&bus->late->armed, 0u)) {
        bus->late->left = 0;
    } else {
        /* a frame has ended this very moment and its interrupt has started the pieces: they are let through, they do no harm */
        dgx_dsi_late_idle(bus);
    }
    bus->late->count = 0;
}

/*
 * A task waits until the display has taken the picture, and nothing is
 * drawn meanwhile. The time is used: the pieces are copied now, when the
 * frame lasts long enough for all of them, and then nothing has to be
 * copied behind the back of the tasks, and the task that draws next does
 * not find a piece in its way.
 */
static void dgx_dsi_late_now(dgx_dsi_bus_t *bus)
{
    if (!bus->late || !bus->late->count || !atomic_load(&bus->late->armed)) return;
    const uint32_t period = bus->refresh_period_us, since = (uint32_t)esp_timer_get_time() - bus->refresh_us;
    uint32_t       bytes = 0;
    for (uint32_t i = 0; i < bus->late->count; ++i) bytes += (uint32_t)bus->late->piece[i].width * bus->late->piece[i].rows * bus->area.pixel_bytes;
    /* nothing else is moved while the task waits, so the copy goes at the speed that was measured; it is to be over before the frame ends */
    const uint32_t takes = (uint32_t)(((uint64_t)bytes * bus->late->mb_us) >> 20) + (bus->late->count + 1u) * bus->tuning.dma_start_us;
    if (!bus->late->mb_us || period < 1000u || period > 100000u || since + takes >= period) return;
    if (!atomic_exchange(&bus->late->armed, 0u)) return; /* a frame has ended this moment: the interrupt has them */
    bus->late->left = 0;
    dgx_dsi_late_by_hand(bus, (1u << bus->late->count) - 1u);
    bus->late->count = 0;
}

/* the display reads the picture: its pieces are being copied into it, or have been */
static void dgx_dsi_late_moved(dgx_dsi_bus_t *bus)
{
    if (!bus->late || !bus->late->count || !atomic_exchange(&bus->late->armed, 0u)) return;
    /* no interrupt has taken them, as when no frames are sent */
    bus->late->left = 0;
    dgx_dsi_late_by_hand(bus, (1u << bus->late->count) - 1u);
    bus->late->count = 0;
}

/* asked by the tiles of every rectangle a finished picture lacks: how many of its rows are to be copied at once */
static uint32_t dgx_dsi_late_take(void *context, uint32_t left, uint32_t top, uint32_t width, uint32_t rows)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)context;
    if (bus->late->count == DGX_DSI_LATE_PIECES) return rows;
    /* the display reads the first rows of a frame as soon as the frame before has ended: nothing is ahead of it there */
    uint32_t now = 0;
    if (top < bus->tuning.scan_lead) {
        const uint32_t tile = 1u << bus->tiles.shift_y;
        now = (bus->tuning.scan_lead - top + tile - 1u) / tile * tile;
    }
    if (now >= rows || (rows - now) * (width + bus->tuning.copy_row_px) < bus->tuning.copy_from) return rows;
    dgx_dsi_late_piece_t *part = &bus->late->piece[bus->late->count++];
    part->left = (uint16_t)left, part->top = (uint16_t)(top + now), part->width = (uint16_t)width, part->rows = (uint16_t)(rows - now);
    return now;
}

/*
 * The picture has been finished but for the pieces left for later, and the
 * display is about to be told of it.
 *
 * A piece stays for later only when it is sure to be ahead of the display:
 * it starts when those above it are done, has to start before the display
 * comes to its first row and to end before it comes to its last one, by
 * the speed the 2D DMA was measured at and a quarter more time. One that
 * is not sure, and every one as long as no speed is known, is copied now.
 * Those that stay remain lacked in the maps of the tiles, and all is made
 * ready for the interrupt to start them.
 */
static void dgx_dsi_late_arm(dgx_dsi_bus_t *bus)
{
    dgx_dsi_late_t *late = bus->late;
    if (!late || !late->count) return;
    const uint32_t bytes  = bus->area.pixel_bytes, period = bus->refresh_period_us;
    const size_t   pitch  = (size_t)bus->area.fb_width * bytes;
    const uint8_t *from   = dgx_dsi_other(bus);
    uint8_t       *to     = bus->area.fb;
    const bool     known  = late->mb_us && period >= 1000u && period <= 100000u;
    uint32_t       before = bus->tuning.dma_start_us, shared = bus->tuning.dma_start_us, kept = 0;
    late->alone           = false;
    for (uint32_t i = 0; i < late->count; ++i) {
        const dgx_dsi_late_piece_t part  = late->piece[i];
        const uint32_t             takes = (uint32_t)(((uint64_t)part.width * part.rows * bytes * late->mb_us) >> 20);
        /* when the display comes to the first row of the piece and when it has read its last one, with the rows it reads ahead */
        const uint32_t enters = known ? (uint32_t)((uint64_t)(bus->lines_before + part.top - bus->tuning.scan_lead) * period / bus->lines_total) : 0;
        const uint32_t leaves = known ? (uint32_t)((uint64_t)(bus->lines_before + part.top + part.rows - bus->tuning.scan_lead) * period / bus->lines_total) : 0;
        if (!known || before >= enters || before + takes + takes / 4u >= leaves) {
            dgx_dsi_late_copy(bus, from, to, &part);
            continue;
        }
        /* would it stay ahead if something else were moved at the same time? If not, such a thing waits until the pieces are there */
        const uint32_t slow = takes * bus->tuning.shared_halves / 2u;
        if (shared >= enters || shared + slow >= leaves) late->alone = true;
        before += takes + takes / 4u;
        shared += slow;
        const size_t at = (size_t)part.top * pitch + (size_t)part.left * bytes, span = (size_t)(part.rows - 1u) * pitch + (size_t)part.width * bytes;
        dgx_fb_tiles_lack(&bus->tiles, part.left, part.top, (uint16_t)(part.left + part.width - 1u), (uint16_t)(part.top + part.rows - 1u));
        /* the DMA reads and writes memory: what the cache holds of the place written is brought out and forgotten */
        esp_cache_msync((void *)(from + at), span, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        esp_cache_msync(to + at, span, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        dgx_dsi_dma_describe(&late->desc[2u * kept], from + (size_t)part.top * pitch, bus->area.fb_width, part.left, part.width, part.rows, (uint8_t)bytes);
        dgx_dsi_dma_describe(&late->desc[2u * kept + 1u], to + (size_t)part.top * pitch, bus->area.fb_width, part.left, part.width, part.rows, (uint8_t)bytes);
        late->piece[kept++] = part;
    }
    late->count = (uint8_t)kept;
    if (!kept) return;
    esp_cache_msync(late->desc, 2u * DGX_DSI_LATE_PIECES * sizeof(dma2d_descriptor_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    late->shared = false;
    late->into   = bus->flip.drawn;
    late->left   = (uint8_t)((1u << kept) - 1u);
    xSemaphoreTake(late->step, 0);
    atomic_store(&late->armed, 1u);
}

static void dgx_dsi_late_free(dgx_dsi_bus_t *bus)
{
    dgx_dsi_late_t *late = bus->late;
    bus->late        = NULL;
    bus->tiles.later = NULL;
    if (!late) return;
    if (late->step) vSemaphoreDelete(late->step);
    free(late->trans);
    free(late->desc);
    free(late);
}

/* with two frame buffers and the 2D DMA at hand; without the memory for it everything is copied at once, as before */
static void dgx_dsi_late_init(dgx_dsi_bus_t *bus)
{
    dgx_dsi_late_t *late = (dgx_dsi_late_t *)heap_caps_calloc(1, sizeof(dgx_dsi_late_t), MALLOC_CAP_INTERNAL);
    if (!late) return;
    late->trans_size = (uint32_t)dma2d_get_trans_elm_size();
    late->desc  = (dma2d_descriptor_t *)heap_caps_aligned_calloc(64, 1, (2u * DGX_DSI_LATE_PIECES * sizeof(dma2d_descriptor_t) + 63u) & ~63u,
                                                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    late->trans = (uint8_t *)heap_caps_calloc(DGX_DSI_LATE_PIECES, late->trans_size, MALLOC_CAP_INTERNAL);
    late->step  = xSemaphoreCreateBinary();
    bus->late   = late;
    if (!late->desc || !late->trans || !late->step) {
        dgx_dsi_late_free(bus);
        return;
    }
    for (uint32_t i = 0; i < DGX_DSI_LATE_PIECES; ++i) {
        late->context[i].bus   = bus;
        late->context[i].piece = (uint8_t)i;
        late->config[i] = (dma2d_trans_config_t){
            .tx_channel_num = 1,
            .rx_channel_num = 1,
            .channel_flags  = DMA2D_CHANNEL_FUNCTION_FLAG_SIBLING,
            .on_job_picked  = dgx_dsi_late_picked,
            .user_config    = &late->context[i],
        };
    }
    bus->tiles.later         = dgx_dsi_late_take;
    bus->tiles.later_context = bus;
}

static inline bool dgx_dsi_late_busy(const dgx_dsi_bus_t *bus)
{
    return bus->late && bus->late->left != 0;
}

/* a task is about to move much memory by other means than the processor */
static void dgx_dsi_late_shares(dgx_dsi_bus_t *bus)
{
    if (!bus->late || !bus->late->left) return;
    if (bus->late->alone) {
        dgx_dsi_late_idle(bus);
    } else {
        bus->late->shared = true;
    }
}
#else
static inline bool dgx_dsi_late_busy(const dgx_dsi_bus_t *bus)
{
    return false;
}

static inline void dgx_dsi_late_shares(dgx_dsi_bus_t *bus)
{
}

static inline void dgx_dsi_late_clear(dgx_dsi_bus_t *bus, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
}

static inline void dgx_dsi_late_idle(dgx_dsi_bus_t *bus)
{
}
#endif

#if DGX_DSI_HARDWARE_COPY
/*
 * Rows of pixels of a program, kept as DGX keeps them, straight into a
 * rectangle of the screen by the 2D DMA. It does what sending them through
 * the window does, several times as fast, when the pixels are 16-bit, the
 * screen lies on the panel as it is and the rectangle is not small.
 */
static bool dgx_dsi_write_rows(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *rows,
                               uint32_t pitch)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->area.fb || bus->area.pixel_bytes != 2 || bus->area.swap_rb || bus->turned || (pitch & 1u)) return false;
    if ((uint32_t)width * height * 2u < bus->tuning.rows_from) return false;
    if ((uint32_t)left + width > bus->area.fb_width || (uint32_t)top + height > bus->area.fb_height) return false;
    if (bus->frames == 2) {
        dgx_dsi_draw_ready(bus);
        if (dgx_dsi_late_busy(bus)) dgx_dsi_late_clear(bus, left, top, (uint16_t)(left + width - 1u), (uint16_t)(top + height - 1u));
        /* every pixel of the rectangle is written: of its tiles only what lies outside it is brought up to date */
        dgx_dsi_tiles_ready(bus, left, left + width - 1, top, top + height - 1, true, true);
    }
    uint8_t *to = bus->area.fb + (size_t)top * bus->area.fb_width * 2u;
    if (!dgx_dsi_dma(bus, rows, pitch / 2u, 0, to, left, width, height, true, bus->after_scan)) return false;
    bus->rows_written = true;
    return true;
}
#endif

static inline void dgx_dsi_written(dgx_dsi_bus_t *bus)
{
    /* with two frame buffers what is drawn is not seen until it is shown: nothing to bring out on the way */
    if (bus->frames != 1 || bus->area.dirty_weight <= DGX_DSI_SHOW_WEIGHT) return;
    /*
     * More than the cache keeps to itself: it goes to the display as it is
     * drawn. Behind the scan that means the batch is not held back at its
     * end either: a part of it is seen, the rest must not come a frame
     * later.
     */
    uint32_t from, length;
    bus->batch_seen = true;
    if (dgx_fb_area_take_dirty(&bus->area, &from, &length)) dgx_dsi_bring_out(bus->area.fb + from, length);
}

static bool dgx_dsi_memory_command(const dgx_dsi_bus_t *bus)
{
    switch (bus->area.command) {
    case DGX_FB_AREA_WRITE:
    case DGX_FB_AREA_WRITE_CONTINUE:
    case DGX_FB_AREA_READ:
    case DGX_FB_AREA_READ_CONTINUE:
        return bus->area.fb != NULL;
    default:
        return false;
    }
}

static void dgx_dsi_send_command(struct _dgx_bus_protocols_t *_bus, uint8_t cmd)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    dgx_dsi_send_pending(bus);
    /* the window is about to change, or to be written from its first pixel again */
    if (bus->window_open && cmd != DGX_FB_AREA_WRITE_CONTINUE) dgx_dsi_window_close(bus);
    /*
     * The frame buffer answers the commands of the window once the display
     * is started. Before that a driver sets its controller up, and there
     * the same numbers may be registers of the controller: JD9165 has 0x2A,
     * 0x2B and 0x2C on its page 1. They go to the display.
     */
    if (bus->started && dgx_fb_area_command(&bus->area, cmd)) return;
    bus->command         = cmd;
    bus->command_pending = true;
}

static void dgx_dsi_send_commands(struct _dgx_bus_protocols_t *_bus, const uint8_t *cmds, uint32_t len)
{
    uint32_t byte_len = (len + 7u) / 8u;
    for (uint32_t i = 0; i < byte_len; i++) {
        dgx_dsi_send_command(_bus, cmds[i]);
    }
    dgx_dsi_send_pending((dgx_dsi_bus_t *)_bus);
}

static void dgx_dsi_send_data(struct _dgx_bus_protocols_t *_bus, const uint8_t *data, uint32_t len)
{
    dgx_dsi_bus_t *bus      = (dgx_dsi_bus_t *)_bus;
    uint32_t       byte_len = (len + 7u) / 8u;
    if (byte_len == 0) return;
    if (bus->area.fb) {
        if (bus->after_scan && bus->area.command == DGX_FB_AREA_WRITE && bus->area.x == bus->area.left && bus->area.y == bus->area.top) {
            dgx_dsi_wait_scan_window(bus);
        }
        if (bus->frames == 2 && (bus->area.command == DGX_FB_AREA_WRITE || bus->area.command == DGX_FB_AREA_WRITE_CONTINUE)) {
            dgx_dsi_draw_ready(bus);
            if (dgx_dsi_late_busy(bus)) {
                uint16_t x0, y0, x1, y1;
                dgx_fb_area_to_panel(&bus->area, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, &x0, &y0, &x1, &y1);
                dgx_dsi_late_clear(bus, x0, y0, x1, y1);
            }
            if (bus->window_open) {
                bus->window_got += byte_len / bus->area.pixel_bytes;
            } else if (bus->area.command == DGX_FB_AREA_WRITE && bus->area.x == bus->area.left && bus->area.y == bus->area.top) {
                /* pixels for the window from its first one on: most likely for all of it, which is known when they end */
                uint16_t x0, y0, x1, y1;
                dgx_fb_area_to_panel(&bus->area, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, &x0, &y0, &x1, &y1);
                dgx_fb_tiles_open(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), x0, y0, x1, y1);
                bus->window_open = true;
                bus->window_got  = byte_len / bus->area.pixel_bytes;
            } else {
                dgx_dsi_tiles_ready(bus, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, false, true);
            }
        }
        if (dgx_fb_area_data(&bus->area, data, byte_len)) return;
    }
    bus->command_pending = false;
    ESP_LOGD(TAG, "command 0x%02x with %u bytes, the first 0x%02x", bus->command, (unsigned)byte_len, data[0]);
    esp_err_t ret        = esp_lcd_panel_io_tx_param(bus->io, bus->command, data, byte_len);
    if (ret != ESP_OK) ESP_LOGE(TAG, "command 0x%02x with %u bytes failed: %s", bus->command, (unsigned)byte_len, esp_err_to_name(ret));
}

static uint32_t dgx_dsi_read_data(struct _dgx_bus_protocols_t *_bus, uint8_t *data, uint32_t len)
{
    dgx_dsi_bus_t *bus      = (dgx_dsi_bus_t *)_bus;
    uint32_t       byte_len = (len + 7u) / 8u;
    if (byte_len == 0) return 0;
    if (dgx_dsi_memory_command(bus)) {
        uint32_t pixels = byte_len / bus->area.pixel_bytes;
        /* a picture that is finished may still lack a piece left for later: it is brought as anything lacked is */
        if (bus->frames == 2) {
            dgx_dsi_tiles_ready(bus, bus->area.left, bus->area.right, bus->area.top, bus->area.bottom, false, false);
            if (bus->flip.told) dgx_dsi_read_out(bus);
        }
        dgx_fb_area_read(&bus->area, data, pixels);
        return pixels * bus->area.pixel_bytes * 8u;
    }
    bus->command_pending = false;
    esp_err_t ret        = esp_lcd_panel_io_rx_param(bus->io, bus->command, data, byte_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reading 0x%02x failed: %s", bus->command, esp_err_to_name(ret));
        memset(data, 0, byte_len);
        return 0;
    }
    return len;
}

static void dgx_dsi_sync_write(struct _dgx_bus_protocols_t *_bus)
{
    dgx_dsi_send_pending((dgx_dsi_bus_t *)_bus);
}

/*
 * Memory for pixels on their way to the display: the buffer of the window and the bands a picture is
 * made ready in for the 2D DMA. Internal memory when there is any: what the processor writes here it
 * should not have to carry through the cache of the PSRAM, where malloc() puts a large block.
 */
static uint8_t *dgx_dsi_memory_allocator(uint32_t len)
{
    uint8_t *memory = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return memory ? memory : (uint8_t *)malloc(len);
}

static void dgx_dsi_dispose_bus_func(struct _dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (bus->panel) esp_lcd_panel_del(bus->panel);
    if (bus->refreshed) vSemaphoreDelete(bus->refreshed);
    if (bus->tiles.owed) dgx_fb_tiles_free(&bus->tiles);
#if DGX_DSI_HARDWARE_COPY
    if (bus->late) {
        atomic_store(&bus->late->armed, 0u);
        dgx_dsi_late_idle(bus);
        dgx_dsi_late_free(bus);
    }
    dgx_dsi_dma_free(bus);
#endif
#if DGX_DSI_HARDWARE_FILL
    if (bus->filler) ppa_unregister_client(bus->filler);
#if DGX_DSI_HARDWARE_MIX
    if (bus->mixer) ppa_unregister_client(bus->mixer);
#endif
#endif
    if (bus->io) esp_lcd_panel_io_del(bus->io);
    if (bus->dsi) esp_lcd_del_dsi_bus(bus->dsi);
    if (bus->phy_power) esp_ldo_release_channel(bus->phy_power);
    free(bus->protocols.buffer);
    free(bus);
}

dgx_bus_protocols_t *dgx_dsi_init(int bus_id, uint8_t data_lanes, uint32_t lane_bit_rate_mbps, int phy_ldo_chan, int phy_ldo_mv)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)calloc(1, sizeof(dgx_dsi_bus_t));
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed to allocate dgx_dsi_bus_t");
        return NULL;
    }
    esp_err_t ret;
    if (phy_ldo_chan >= 0) {
        /* without it the PHY has no power and the bus cannot be made */
        esp_ldo_channel_config_t ldo_config = {
            .chan_id    = phy_ldo_chan,
            .voltage_mv = phy_ldo_mv,
        };
        ret = esp_ldo_acquire_channel(&ldo_config, &bus->phy_power);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ldo_acquire_channel failed: %s", esp_err_to_name(ret));
            dgx_dsi_dispose_bus_func(&bus->protocols);
            return NULL;
        }
    }
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id             = bus_id,
        .num_data_lanes     = data_lanes,
        .phy_clk_src        = 0, /* the default of the chip revision */
        .lane_bit_rate_mbps = lane_bit_rate_mbps,
    };
    ret = esp_lcd_new_dsi_bus(&bus_config, &bus->dsi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_dsi_bus failed: %s", esp_err_to_name(ret));
        dgx_dsi_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    esp_lcd_dbi_io_config_t io_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(bus->dsi, &io_config, &bus->io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_dbi failed: %s", esp_err_to_name(ret));
        dgx_dsi_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    bus->data_lanes                 = data_lanes;
    bus->tile_shift_x               = (uint8_t)DGX_DSI_TILE_SHIFT_X;
    bus->tile_shift_y               = (uint8_t)DGX_DSI_TILE_SHIFT_Y;
    bus->tuning = (dgx_dsi_tuning_t){
        .version        = DGX_DSI_TUNING_VERSION,
        .size           = sizeof(dgx_dsi_tuning_t),
        .scan_lead      = DGX_DSI_SCAN_LEAD,
        .shared_halves  = DGX_DSI_LATE_SHARED_HALVES,
        .window_kb_a_ms = DGX_DSI_IN_KB_A_MS,
        .out_kb_a_ms    = DGX_DSI_OUT_KB_A_MS,
        .dma_start_us   = DGX_DSI_DMA_START_US,
        .scan_room_us   = DGX_DSI_SCAN_ROOM_US,
        .guard_us       = DGX_DSI_FLIP_GUARD_US,
        .copy_row_px    = DGX_DSI_HARDWARE_COPY_ROW_PX,
        .fill_sum       = DGX_DSI_HARDWARE_FILL_SUM,
        .fill_div       = DGX_DSI_HARDWARE_FILL_DIV,
        .fill_from      = DGX_DSI_HARDWARE_FILL_FROM,
        .copy_from      = DGX_DSI_HARDWARE_COPY_FROM,
        .rows_from      = DGX_DSI_HARDWARE_ROWS_FROM,
    };
    bus->protocols.bus_type         = DGX_BUS_DSI;
    bus->protocols.bus_name         = "DSI";
    bus->protocols.write_command    = dgx_dsi_send_command;
    bus->protocols.write_commands   = dgx_dsi_send_commands;
    bus->protocols.read_data        = dgx_dsi_read_data;
    bus->protocols.write_data       = dgx_dsi_send_data;
    bus->protocols.write_data_async = dgx_dsi_send_data; /* a copy into the frame buffer: done when it returns */
    bus->protocols.sync_write       = dgx_dsi_sync_write;
    bus->protocols.dispose          = dgx_dsi_dispose_bus_func;
    bus->protocols.memory_allocator = dgx_dsi_memory_allocator;
    bus->protocols.buffer_len       = DGX_DSI_BUFFER_LEN;
    bus->protocols.buffer           = bus->protocols.memory_allocator(bus->protocols.buffer_len);
    if (!bus->protocols.buffer) {
        ESP_LOGE(TAG, "dgx_dsi_init buffer allocation failed");
        dgx_dsi_dispose_bus_func(&bus->protocols);
        return NULL;
    }
    return (dgx_bus_protocols_t *)bus;
}

bool dgx_dsi_video_setup(dgx_bus_protocols_t *_bus, const dgx_dsi_video_t *video, uint8_t color_bits, bool swap_rb, dgx_dsi_frames_t frames)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (bus->panel) {
        ESP_LOGE(TAG, "the bus already has a display");
        return false;
    }
    if (color_bits != 16 && color_bits != 24) {
        ESP_LOGE(TAG, "%u bits a pixel: only 16 and 24 are supported", color_bits);
        return false;
    }
    if (frames != DgxDsiOneFrame && frames != DgxDsiTwoFrames && frames != DgxDsiOneFrameAfterScan) {
        ESP_LOGE(TAG, "%d is not a way to update the display", (int)frames);
        return false;
    }
    const bool two = frames == DgxDsiTwoFrames, after_scan = frames == DgxDsiOneFrameAfterScan;
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel    = 0,
        .dpi_clk_src        = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = (float)video->pixel_clock_khz / 1000.0f,
        .in_color_format    = color_bits == 16 ? LCD_COLOR_FMT_RGB565 : LCD_COLOR_FMT_RGB888,
        .num_fbs            = two ? 2 : 1,
        .video_timing = {
            .h_size            = video->width,
            .v_size            = video->height,
            .hsync_pulse_width = video->hsync_pulse_width,
            .hsync_back_porch  = video->hsync_back_porch,
            .hsync_front_porch = video->hsync_front_porch,
            .vsync_pulse_width = video->vsync_pulse_width,
            .vsync_back_porch  = video->vsync_back_porch,
            .vsync_front_porch = video->vsync_front_porch,
        },
    };
    esp_err_t ret = esp_lcd_new_panel_dpi(bus->dsi, &dpi_config, &bus->panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_dpi failed: %s", esp_err_to_name(ret));
        bus->panel = NULL;
        return false;
    }
    void *fb = NULL, *fb2 = NULL;
    ret      = two ? esp_lcd_dpi_panel_get_frame_buffer(bus->panel, 2, &fb, &fb2) : esp_lcd_dpi_panel_get_frame_buffer(bus->panel, 1, &fb);
    if (ret == ESP_OK && (two || after_scan)) {
        /* told where a frame ends: to know when the display has changed the frame buffer it reads, or where its scan is */
        bus->refreshed = xSemaphoreCreateBinary();
#if DGX_DSI_FRAME_BUF_COMPLETE
        esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_frame_buf_complete = dgx_dsi_on_refresh};
#else
        esp_lcd_dpi_panel_event_callbacks_t callbacks = {.on_refresh_done = dgx_dsi_on_refresh};
#endif
        ret = bus->refreshed ? esp_lcd_dpi_panel_register_event_callbacks(bus->panel, &callbacks, bus) : ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK && two && !dgx_fb_tiles_init(&bus->tiles, video->width, video->height, color_bits / 8u, bus->tile_shift_x, bus->tile_shift_y)) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret != ESP_OK || !fb || (two && !fb2)) {
        ESP_LOGE(TAG, "the frame buffers of the display: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(bus->panel);
        bus->panel = NULL;
        if (bus->refreshed) vSemaphoreDelete(bus->refreshed);
        bus->refreshed = NULL;
        if (bus->tiles.owed) dgx_fb_tiles_free(&bus->tiles);
        return false;
    }
    bus->frames      = two ? 2 : 1;
    bus->after_scan  = after_scan;
    bus->tuning.width       = video->width;
    bus->tuning.height      = video->height;
    bus->tuning.pixel_bytes = (uint8_t)(color_bits / 8u);
    if (two) bus->tile_shift_x = bus->tiles.shift_x; /* as wide as the panel made it */
    bus->lines_total = (uint16_t)(video->height + video->vsync_front_porch + video->vsync_pulse_width + video->vsync_back_porch);
    {
        const uint64_t row = (uint64_t)video->width + video->hsync_front_porch + video->hsync_pulse_width + video->hsync_back_porch;
        bus->frame_us_256  = video->pixel_clock_khz ? (uint32_t)(row * bus->lines_total * 256000u / video->pixel_clock_khz) : 0;
    }
    /*
     * The end of a frame is counted where the last row has been read, on a
     * chip of revision 3 and later at the vertical sync that follows.
     */
#if DGX_DSI_REFRESH_AT_SWITCH
    bus->lines_before = (uint16_t)(video->vsync_front_porch + video->vsync_pulse_width + video->vsync_back_porch);
#else
    bus->lines_before = (uint16_t)(video->vsync_pulse_width + video->vsync_back_porch);
#endif
#if DGX_DSI_HARDWARE_COPY
    dgx_dsi_dma_init(bus);
    if (bus->dma_pool) {
        bus->protocols.write_rows = dgx_dsi_write_rows;
        if (two) {
            bus->tiles.copier         = dgx_dsi_hardware_copy;
            bus->tiles.copier_context = bus;
            /*
             * At 24 bits a row is half as many bytes more, and so the
             * display reads half as fast again, while the 2D DMA copies as
             * it does: the pieces fell behind the scan now and then. So
             * there everything a picture lacks is copied when it is
             * finished.
             */
            if (color_bits == 16) dgx_dsi_late_init(bus);
        }
    }
#endif
#if DGX_DSI_HARDWARE_FILL
    {
        const ppa_client_config_t filler_config = {.oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1};
        if (ppa_register_client(&filler_config, &bus->filler) != ESP_OK) {
            ESP_LOGW(TAG, "no accelerator to fill rectangles: the processor fills");
            bus->filler = NULL;
        }
#if DGX_DSI_HARDWARE_MIX
        const ppa_client_config_t mixer_config = {.oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1};
        if (bus->filler && color_bits == 16 && ppa_register_client(&mixer_config, &bus->mixer) == ESP_OK) {
            bus->protocols.write_ink = dgx_dsi_write_ink;
        } else {
            bus->mixer = NULL;
        }
#endif
    }
#endif
    bus->frame[0] = (uint8_t *)fb;
    bus->frame[1] = (uint8_t *)fb2;
    /* the display starts with the first one: with two, drawing goes into the second */
#if DGX_DSI_REFRESH_AT_SWITCH
    dgx_fb_flip_init(&bus->flip, &dgx_dsi_flip_display, bus, two ? 1 : 0, true, bus->tuning.guard_us);
#else
    dgx_fb_flip_init(&bus->flip, &dgx_dsi_flip_display, bus, two ? 1 : 0, false, bus->tuning.guard_us);
#endif
    dgx_fb_area_init(&bus->area, bus->frame[bus->flip.drawn], video->width, video->height, color_bits / 8u, swap_rb);
    return true;
}

bool dgx_dsi_video_start(dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->panel) {
        ESP_LOGE(TAG, "dgx_dsi_video_start before dgx_dsi_video_setup");
        return false;
    }
    dgx_dsi_send_pending(bus);
    esp_err_t ret = esp_lcd_panel_init(bus->panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(ret));
        return false;
    }
    bus->started = true;
    return true;
}

void dgx_dsi_orientation(dgx_bus_protocols_t *_bus, bool mirror_x, bool mirror_y, bool swap_xy)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    dgx_dsi_window_close(bus); /* where the window lies on the panel is told by the orientation it was sent in */
    dgx_fb_area_orientation(&bus->area, mirror_x, mirror_y, swap_xy);
    bus->turned = mirror_x || mirror_y || swap_xy;
}

void dgx_dsi_show(dgx_bus_protocols_t *_bus)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    uint32_t       from, length;
    const bool     drawn = dgx_fb_area_take_dirty(&bus->area, &from, &length);
    if (drawn) {
        if (bus->after_scan && !bus->batch_seen) dgx_dsi_wait_scan_bytes(bus, from, length);
        dgx_dsi_bring_out(bus->area.fb + from, length);
    }
    bus->batch_seen = false;
    /* what the 2D DMA has put is in memory already, but with two frame buffers it is still to be shown */
    if (!drawn && !bus->rows_written) return;
    bus->rows_written = false;
    ++bus->batches;
    if (bus->frames == 2) {
        /*
         * The picture is finished: the frame buffer gets what it still
         * lacks, but for the large pieces, which are left for after the
         * display has taken it, and the display is told that there is
         * something to show.
         */
        dgx_dsi_window_close(bus);
        dgx_fb_flip_drawn(&bus->flip);
#if DGX_DSI_HARDWARE_COPY
        /* the pieces of the picture before go by the same means: they have to be there first */
        dgx_dsi_late_idle(bus);
        if (bus->late) bus->late->count = 0;
#endif
        dgx_fb_tiles_complete(&bus->tiles, bus->area.fb, dgx_dsi_other(bus));
        if (dgx_fb_tiles_take_copied(&bus->tiles, &from, &length)) dgx_dsi_bring_out(bus->area.fb + from, length);
#if DGX_DSI_HARDWARE_COPY
        dgx_dsi_late_arm(bus);
#endif
        dgx_fb_flip_finished(&bus->flip);
    }
}

uint8_t dgx_dsi_data_lanes(dgx_bus_protocols_t *_bus)
{
    return ((dgx_dsi_bus_t *)_bus)->data_lanes;
}

void dgx_dsi_counters(dgx_bus_protocols_t *_bus, dgx_dsi_counters_t *counters)
{
    const dgx_dsi_bus_t *bus = (const dgx_dsi_bus_t *)_bus;
    counters->frames       = bus->refreshes;
    counters->batches      = bus->batches;
    counters->flips        = bus->flip.flips;
    counters->joined       = bus->flip.joined;
    counters->kept_back    = bus->flip.kept_back;
    counters->flip_waits   = bus->flip.waits;
    counters->flip_wait_us = bus->flip.wait_us;
    counters->scan_waits   = bus->scan_waits;
    counters->scan_wait_us = bus->scan_wait_us;
    counters->copied_bytes = bus->tiles.copied_bytes;
    counters->copied_late_bytes = counters->copied_late_behind = counters->copy_waits = counters->copy_wait_us = counters->copy_kb_a_ms = 0;
#if DGX_DSI_HARDWARE_COPY
    if (bus->late) {
        counters->copied_bytes += bus->late->bytes;
        counters->copied_late_bytes  = bus->late->bytes;
        counters->copied_late_behind = bus->late->behind;
        counters->copy_waits         = bus->late->waits;
        counters->copy_wait_us       = bus->late->wait_us;
        counters->copy_kb_a_ms       = bus->late->mb_us ? (1024u * 1000u) / bus->late->mb_us : 0;
    }
#endif
}

#if DGX_DSI_HARDWARE_FILL
/*
 * Fill by the accelerator the columns x0..x1 of the rows y0..y1 of a picture
 * that has the rows of the panel, `rows` of them: a frame buffer, or the
 * memory dgx_dsi_bench() measures in. `behind_scan` makes it start when the
 * display is not reading the rows. Returns false when it has not been done.
 */
static bool dgx_dsi_accelerator_fill(dgx_dsi_bus_t *bus, uint8_t *picture, uint32_t rows, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color,
                                     bool behind_scan)
{
    const dgx_fb_area_t *area  = &bus->area;
    const uint32_t       bytes = area->pixel_bytes;
    /* the color as 8 bits of red, green and blue; 5 and 6 bits are spread so that the accelerator gets the same 5 and 6 back */
    uint32_t r, g, b;
    if (bytes == 2) {
        r = (color >> 11) & 0x1fu, g = (color >> 5) & 0x3fu, b = color & 0x1fu;
        r = r << 3 | r >> 2, g = g << 2 | g >> 4, b = b << 3 | b >> 2;
    } else {
        r = (color >> 16) & 0xffu, g = (color >> 8) & 0xffu, b = color & 0xffu;
    }
    const size_t pitch = (size_t)area->fb_width * bytes;
    uint8_t     *first = picture + (size_t)y0 * pitch + (size_t)x0 * bytes;
    const size_t span  = (size_t)(y1 - y0) * pitch + (size_t)(x1 - x0 + 1u) * bytes;
    /* what the cache holds of the place goes out before the wait for the scan, not after it */
    esp_cache_msync(first, span, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    /* the accelerator fills four times as fast as the display reads: started when the scan is not in the rows, it stays ahead of it */
    if (behind_scan) dgx_dsi_wait_scan_outside(bus, y0, y1);
    const ppa_fill_oper_config_t fill = {
        .out = {
            .buffer         = picture,
            .buffer_size    = (uint32_t)(pitch * rows),
            .pic_w          = area->fb_width,
            .pic_h          = rows,
            .block_offset_x = x0,
            .block_offset_y = y0,
            .fill_cm        = bytes == 2 ? PPA_FILL_COLOR_MODE_RGB565 : PPA_FILL_COLOR_MODE_RGB888,
        },
        .fill_block_w    = x1 - x0 + 1u,
        .fill_block_h    = y1 - y0 + 1u,
        .fill_argb_color = {.a = 0xff, .r = area->swap_rb ? b : r, .g = g, .b = area->swap_rb ? r : b},
        .mode            = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(bus->filler, &fill) == ESP_OK;
}

#if DGX_DSI_HARDWARE_MIX
/* a color of 16 bits as the 8 bits of red, green and blue the accelerator takes, the screen's own order of red and blue minded */
static color_pixel_rgb888_data_t dgx_dsi_color_888(const dgx_dsi_bus_t *bus, uint32_t color)
{
    uint32_t r = (color >> 11) & 0x1fu, g = (color >> 5) & 0x3fu, b = color & 0x1fu;
    r = r << 3 | r >> 2, g = g << 2 | g >> 4, b = b << 3 | b >> 2;
    const color_pixel_rgb888_data_t rgb = {.r = (uint8_t)(bus->area.swap_rb ? b : r), .g = (uint8_t)g, .b = (uint8_t)(bus->area.swap_rb ? r : b)};
    return rgb;
}

/*
 * A picture of amounts of ink put into a rectangle of the frame buffer by the accelerator: the rectangle
 * is filled with the color of the paper, and then the ink is mixed into it, the picture being how
 * opaque the ink is in every pixel. The processor carries no pixel. Returns false when it has not been
 * done; the rectangle may be left filled with paper then, and is to be sent whole another way.
 */
static bool dgx_dsi_write_ink(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, const uint8_t *ink,
                              uint32_t pitch, uint32_t paper, uint32_t color)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->mixer || !bus->filler || !bus->area.fb || bus->area.pixel_bytes != 2 || bus->turned || pitch < width) return false;
    /*
     * Between the paper and the ink the rectangle is paper alone. With two frame buffers nobody sees
     * it; with one the display would show it now and then, so there it is not done this way.
     */
    if (bus->frames != 2) return false;
    if ((uint32_t)left + width > bus->area.fb_width || (uint32_t)top + height > bus->area.fb_height) return false;
    dgx_dsi_draw_ready(bus);
    if (dgx_dsi_late_busy(bus)) dgx_dsi_late_clear(bus, left, top, (uint16_t)(left + width - 1u), (uint16_t)(top + height - 1u));
    /* every pixel of the rectangle is written: of its tiles only what lies outside it is brought up to date */
    dgx_dsi_tiles_ready(bus, left, left + width - 1, top, top + height - 1, true, true);
    dgx_dsi_late_shares(bus);
    if (!dgx_dsi_accelerator_fill(bus, bus->area.fb, bus->area.fb_height, left, top, left + width - 1u, top + height - 1u, paper, bus->after_scan)) return false;
    const uint32_t                size = (uint32_t)bus->area.fb_width * bus->area.fb_height * 2u;
    const ppa_blend_oper_config_t mix = {
        .in_bg = {
            .buffer         = bus->area.fb,
            .pic_w          = bus->area.fb_width,
            .pic_h          = bus->area.fb_height,
            .block_w        = width,
            .block_h        = height,
            .block_offset_x = left,
            .block_offset_y = top,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer   = ink,
            .pic_w    = pitch,
            .pic_h    = height,
            .block_w  = width,
            .block_h  = height,
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer         = bus->area.fb,
            .buffer_size    = size,
            .pic_w          = bus->area.fb_width,
            .pic_h          = bus->area.fb_height,
            .block_offset_x = left,
            .block_offset_y = top,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
        .bg_alpha_fix_val     = 255,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_fix_rgb_val       = dgx_dsi_color_888(bus, color),
        .mode                 = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_blend(bus->mixer, &mix) != ESP_OK) return false;
    bus->rows_written = true;
    return true;
}
#endif /* DGX_DSI_HARDWARE_MIX */

/*
 * Fill a rectangle of the screen, which lies inside it, by the accelerator.
 * Returns false when it has not been done: the processor fills then.
 */
static bool dgx_dsi_hardware_fill(dgx_dsi_bus_t *bus, int x, int y, int w, int h, uint32_t color)
{
    uint16_t x0, y0, x1, y1;
    dgx_fb_area_to_panel(&bus->area, x, x + w - 1, y, y + h - 1, &x0, &y0, &x1, &y1);
    /* pieces that are only just ahead of the scan of the display are not slowed down by a fill */
    dgx_dsi_late_shares(bus);
    return dgx_dsi_accelerator_fill(bus, bus->area.fb, bus->area.fb_height, x0, y0, x1, y1, color, bus->after_scan);
}
#endif

uint32_t dgx_dsi_frame_us(dgx_bus_protocols_t *_bus)
{
    return (((const dgx_dsi_bus_t *)_bus)->frame_us_256 + 128u) >> 8;
}

bool dgx_dsi_frame_ended(dgx_bus_protocols_t *_bus, int64_t *ended_us)
{
    const dgx_dsi_bus_t *bus = (const dgx_dsi_bus_t *)_bus;
    if (!bus->refreshed || !bus->refreshes) return false;
    /* the time of the end first: a frame that ends between the two readings leaves the one before it, which is still an end of a frame */
    const uint32_t ended = bus->refresh_us;
    const int64_t  now   = esp_timer_get_time();
    const uint32_t since = (uint32_t)now - ended;
    /* no frame for several frames: the display was stopped, and where its frames end now is not known */
    if (since > 4u * (((bus->frame_us_256 + 128u) >> 8) + 1u)) return false;
    *ended_us = now - (int64_t)since;
    return true;
}

void dgx_dsi_tuning(dgx_bus_protocols_t *_bus, dgx_dsi_tuning_t *tuning)
{
    *tuning = ((const dgx_dsi_bus_t *)_bus)->tuning;
}

/*
 * Tiles of another size, between two batches. The maps tell what the frame
 * buffer drawing goes into lacks and what was drawn in it, and neither can
 * be told by tiles of another size. So first there is nothing to tell: the
 * picture that is finished goes to the display, the frame buffer that
 * becomes free gets all it lacks, and nothing is drawn in it yet. Then the
 * new maps, which are empty, are right.
 */
static esp_err_t dgx_dsi_tiles_resize(dgx_dsi_bus_t *bus, uint8_t shift_x, uint8_t shift_y)
{
    dgx_fb_tiles_t tiles;
    if (bus->frames != 2) return ESP_OK;
    if (!dgx_fb_tiles_init(&tiles, bus->tiles.width, bus->tiles.height, bus->tiles.pixel_bytes, shift_x, shift_y)) return ESP_ERR_NO_MEM;
    if (tiles.shift_x == bus->tiles.shift_x && tiles.shift_y == bus->tiles.shift_y) {
        dgx_fb_tiles_free(&tiles);
        return ESP_OK;
    }
    dgx_dsi_window_close(bus);
    if (bus->flip.told) {
        /* no more is drawn into the picture: the display takes it, and the wait is for that */
        bus->flip.closed = true;
        dgx_dsi_draw_moved(bus);
        /* the time from here to the next drawing is not the time of a batch */
        bus->flip.timed = false;
    }
    for (uint32_t row = 0; row < bus->tiles.rows; ++row) {
        if (bus->tiles.fresh[row]) {
            /* a batch is under way: what it drew cannot be told from what it lacks without the maps */
            dgx_fb_tiles_free(&tiles);
            return ESP_ERR_INVALID_STATE;
        }
    }
#if DGX_DSI_HARDWARE_COPY
    /* what the picture on the display was left lacking has to be there first, and nothing more is left for later */
    dgx_dsi_late_idle(bus);
    if (bus->late) bus->late->count = 0;
#endif
    const dgx_fb_tiles_later_t later = bus->tiles.later;
    uint32_t                   from, length;
    bus->tiles.later                 = NULL;
    dgx_fb_tiles_complete(&bus->tiles, bus->area.fb, dgx_dsi_other(bus));
    if (dgx_fb_tiles_take_copied(&bus->tiles, &from, &length)) dgx_dsi_bring_out(bus->area.fb + from, length);
    tiles.copier         = bus->tiles.copier;
    tiles.copier_context = bus->tiles.copier_context;
    tiles.later          = later;
    tiles.later_context  = bus->tiles.later_context;
    tiles.copied_bytes   = bus->tiles.copied_bytes;
    dgx_fb_tiles_free(&bus->tiles);
    bus->tiles = tiles;
    return ESP_OK;
}

esp_err_t dgx_dsi_tiles(dgx_bus_protocols_t *_bus, uint16_t width, uint16_t height)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus) return ESP_ERR_INVALID_ARG;
    if (width < DGX_DSI_TILE_WIDTH_LEAST || width > DGX_DSI_TILE_WIDTH_MOST || (width & (width - 1u)) || !height || height > DGX_DSI_TILE_HEIGHT_MOST ||
        (height & (height - 1u))) {
        ESP_LOGW(TAG, "dgx_dsi_tiles: %ux%u is not a size of tiles, they stay as they are", width, height);
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t shift_x = (uint8_t)__builtin_ctz(width), shift_y = (uint8_t)__builtin_ctz(height);
    /* a display made already has maps: they are made anew */
    const esp_err_t ret = bus->panel ? dgx_dsi_tiles_resize(bus, shift_x, shift_y) : ESP_OK;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "dgx_dsi_tiles: %s, the tiles stay as they are", ret == ESP_ERR_NO_MEM ? "no memory for the maps" : "a batch of drawing is under way");
        return ret;
    }
    bus->tile_shift_x = bus->panel && bus->frames == 2 ? bus->tiles.shift_x : shift_x;
    bus->tile_shift_y = shift_y;
    return ESP_OK;
}

bool dgx_dsi_tune(dgx_bus_protocols_t *_bus, const dgx_dsi_tuning_t *tuning)
{
    dgx_dsi_bus_t          *bus = (dgx_dsi_bus_t *)_bus;
    const dgx_dsi_tuning_t *has = &bus->tuning;
    const char *why = NULL;
    if (!bus->panel) {
        why = "the bus has no display yet";
    } else if (tuning->version != DGX_DSI_TUNING_VERSION || tuning->size != sizeof(dgx_dsi_tuning_t)) {
        why = "they are of another version of DGX";
    } else if (tuning->width != has->width || tuning->height != has->height || tuning->pixel_bytes != has->pixel_bytes) {
        why = "they are of another panel or color depth";
    } else if (!tuning->window_kb_a_ms || !tuning->out_kb_a_ms || !tuning->fill_div || tuning->shared_halves < 2u || tuning->scan_lead >= tuning->height ||
               tuning->guard_us >= 20000u || tuning->dma_start_us >= 20000u || tuning->scan_room_us >= 20000u) {
        /* what the bus divides by, and what would make it wait for ever or never */
        why = "one of them makes no sense";
    }
    if (why) {
        ESP_LOGW(TAG, "dgx_dsi_tune: the numbers are not taken, %s", why);
        return false;
    }
    bus->tuning        = *tuning;
    bus->flip.guard_us = tuning->guard_us;
#if DGX_DSI_HARDWARE_COPY
    /* pieces are left for later from the first picture on, not from the first copy that was timed */
    if (bus->late && tuning->dma_kb_a_ms) bus->late->mb_us = 1024000u / tuning->dma_kb_a_ms;
#endif
    return true;
}

/*
 * dgx_dsi_bench(): two pictures of the width of the panel and 128 rows, in
 * memory of the kind the frame buffer is in. What the bus does to the frame
 * buffers it does to them, a few shapes of each, and the time is taken: the
 * middle one of nine goes, or the longest of three where the number is one
 * to be on the safe side of. Every go starts with nothing of the pictures
 * in the cache, and ends with what it wrote brought out of it.
 */
#define DGX_DSI_BENCH_ROWS  128u
#define DGX_DSI_BENCH_GOES  9
#define DGX_DSI_BENCH_ALIGN 128u

typedef struct {
    dgx_dsi_bus_t *bus;
    dgx_fb_area_t  area;   /* the picture written, as a frame buffer of its own */
    uint8_t       *from, *to;
    uint8_t       *pixels; /* DGX_DSI_BUFFER_LEN bytes as a screen sends them through the window */
    uint32_t       width, bytes;
    size_t         pitch, size;
} dgx_dsi_bench_t;

typedef bool (*dgx_dsi_bench_go_t)(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows);

static void dgx_dsi_bench_forget(const dgx_dsi_bench_t *bench)
{
    esp_cache_msync(bench->from, bench->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cache_msync(bench->to, bench->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

/* what was written into the picture goes out of the cache, as it does when a batch ends */
static void dgx_dsi_bench_out(dgx_dsi_bench_t *bench)
{
    uint32_t from, length;
    if (dgx_fb_area_take_dirty(&bench->area, &from, &length)) dgx_dsi_bring_out(bench->to + from, length);
}

#if DGX_DSI_HARDWARE_COPY
/* the processor copies a rectangle as the tiles do */
static bool dgx_dsi_bench_copy(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; ++y) memcpy(bench->to + y * bench->pitch, bench->from + y * bench->pitch, (size_t)width * bench->bytes);
    dgx_dsi_bring_out(bench->to, (size_t)(rows - 1u) * bench->pitch + (size_t)width * bench->bytes);
    return true;
}

static bool dgx_dsi_bench_dma(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows)
{
    return dgx_dsi_dma(bench->bus, bench->from, bench->width, 0, bench->to, 0, width, rows, false, false);
}
#endif

#if DGX_DSI_HARDWARE_FILL
/* the processor fills a rectangle */
static bool dgx_dsi_bench_fill(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows)
{
    dgx_fb_area_fill(&bench->area, 0, 0, (uint16_t)width, (uint16_t)rows, 0x18c3u);
    dgx_dsi_bench_out(bench);
    return true;
}

static bool dgx_dsi_bench_accelerator(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows)
{
    return dgx_dsi_accelerator_fill(bench->bus, bench->to, DGX_DSI_BENCH_ROWS, 0, 0, width - 1u, rows - 1u, 0x18c3u, false);
}
#endif

/*
 * Pixels come through a window, a buffer of the bus at a time, and the
 * buffer is filled from the other picture, as a virtual screen kept in the
 * same kind of memory fills it.
 */
static bool dgx_dsi_bench_window(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows)
{
    const uint8_t *from = bench->from;
    dgx_fb_area_set_columns(&bench->area, 0, (uint16_t)(width - 1u));
    dgx_fb_area_set_rows(&bench->area, 0, (uint16_t)(rows - 1u));
    dgx_fb_area_rewind(&bench->area);
    for (uint32_t left = width * rows; left;) {
        const uint32_t pixels = left < DGX_DSI_BUFFER_LEN / bench->bytes ? left : DGX_DSI_BUFFER_LEN / bench->bytes;
        memcpy(bench->pixels, from, (size_t)pixels * bench->bytes);
        dgx_fb_area_write(&bench->area, bench->pixels, pixels);
        from += (size_t)pixels * bench->bytes;
        left -= pixels;
    }
    dgx_dsi_bench_out(bench);
    return true;
}

/* the cache brings out what was written: the writing itself is not timed, see dgx_dsi_bench_time() */
static bool dgx_dsi_bench_bring_out(dgx_dsi_bench_t *bench, uint32_t width, uint32_t rows)
{
    dgx_dsi_bring_out(bench->to, (size_t)width * rows * bench->bytes);
    return true;
}

/* microseconds a go takes: the middle one of nine goes, or the longest of three; 0 when it cannot be done */
static uint32_t dgx_dsi_bench_time(dgx_dsi_bench_t *bench, dgx_dsi_bench_go_t go, uint32_t width, uint32_t rows, bool longest)
{
    uint32_t  took[DGX_DSI_BENCH_GOES];
    const int goes = longest ? 3 : DGX_DSI_BENCH_GOES;
    for (int i = 0; i < goes; ++i) {
        dgx_dsi_bench_forget(bench);
        if (go == dgx_dsi_bench_bring_out) memset(bench->to, i, (size_t)width * rows * bench->bytes);
        const uint32_t from = (uint32_t)esp_timer_get_time();
        if (!go(bench, width, rows)) return 0;
        uint32_t us = (uint32_t)esp_timer_get_time() - from;
        if (!us) us = 1;
        int at = i;
        for (; at > 0 && took[at - 1] > us; --at) took[at] = took[at - 1];
        took[at] = us;
    }
    return took[longest ? goes - 1 : goes / 2];
}

#if DGX_DSI_HARDWARE_COPY || DGX_DSI_HARDWARE_FILL
/*
 * Two shapes took `a` and `b` microseconds: what a row and what a byte of
 * them cost, in 1/65536 us. A cost that comes out below nothing is nothing.
 */
static void dgx_dsi_bench_costs(uint32_t a, uint32_t a_rows, uint32_t a_bytes, uint32_t b, uint32_t b_rows, uint32_t b_bytes, int64_t *row, int64_t *byte)
{
    const int64_t det = (int64_t)a_rows * b_bytes - (int64_t)b_rows * a_bytes;
    *row              = (((int64_t)a * b_bytes - (int64_t)b * a_bytes) << 16) / det;
    *byte             = (((int64_t)b * a_rows - (int64_t)a * b_rows) << 16) / det;
    if (*row < 0) *row = 0;
    if (*byte < 0) *byte = 0;
}
#endif

static inline uint32_t dgx_dsi_bench_within(int64_t value, uint32_t from, uint32_t to)
{
    return value < (int64_t)from ? from : value > (int64_t)to ? to : (uint32_t)value;
}

bool dgx_dsi_bench(dgx_bus_protocols_t *_bus, dgx_dsi_tuning_t *tuning)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->panel || !bus->started) {
        ESP_LOGE(TAG, "dgx_dsi_bench before dgx_dsi_video_start");
        return false;
    }
    dgx_dsi_bench_t bench = {.bus = bus, .width = bus->area.fb_width, .bytes = bus->area.pixel_bytes};
    bench.pitch           = (size_t)bench.width * bench.bytes;
    bench.size            = (bench.pitch * DGX_DSI_BENCH_ROWS + DGX_DSI_BENCH_ALIGN - 1u) & ~(size_t)(DGX_DSI_BENCH_ALIGN - 1u);
    const uint32_t caps   = esp_ptr_external_ram(bus->frame[0]) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    bench.from            = (uint8_t *)heap_caps_aligned_alloc(DGX_DSI_BENCH_ALIGN, bench.size, caps);
    bench.to              = (uint8_t *)heap_caps_aligned_alloc(DGX_DSI_BENCH_ALIGN, bench.size, caps);
    bench.pixels          = (uint8_t *)heap_caps_malloc(DGX_DSI_BUFFER_LEN, MALLOC_CAP_INTERNAL);
    if (!bench.from || !bench.to || !bench.pixels) {
        ESP_LOGE(TAG, "dgx_dsi_bench: no memory");
        heap_caps_free(bench.from);
        heap_caps_free(bench.to);
        heap_caps_free(bench.pixels);
        return false;
    }
    /* what a finished picture still lacks is not copied in the middle of the measuring */
    dgx_dsi_late_idle(bus);
    memset(bench.from, 0x5a, bench.size);
    memset(bench.to, 0, bench.size);
    memset(bench.pixels, 0xa5, DGX_DSI_BUFFER_LEN);
    dgx_fb_area_init(&bench.area, bench.to, (uint16_t)bench.width, DGX_DSI_BENCH_ROWS, (uint8_t)bench.bytes, bus->area.swap_rb);
    *tuning = bus->tuning;

    const uint32_t bytes = bench.bytes, wide = bench.width, all = DGX_DSI_BENCH_ROWS;
#if DGX_DSI_HARDWARE_COPY || DGX_DSI_HARDWARE_FILL
    const uint32_t half = wide / 2u, narrow = wide < 32u ? wide : 32u;
#endif

    /* through the window: the whole picture, more than the cache keeps to itself */
    const uint32_t window = dgx_dsi_bench_time(&bench, dgx_dsi_bench_window, wide, all, true);
    /* what a millisecond takes in, less a quarter */
    tuning->window_kb_a_ms = (uint16_t)dgx_dsi_bench_within((int64_t)wide * all * bytes * 750 / 1024 / window, 1, 65535);

    /* out of the cache: as much as it surely holds */
    const uint32_t kept = 32u * 1024u < wide * all * bytes ? 32u * 1024u : wide * all * bytes;
    const uint32_t out  = dgx_dsi_bench_time(&bench, dgx_dsi_bench_bring_out, kept / bytes, 1, true);
    tuning->out_kb_a_ms = (uint16_t)dgx_dsi_bench_within((int64_t)kept * 750 / 1024 / out, 1, 65535);
    ESP_LOGD(TAG, "bench: a window of %ux%u in %u us, %u KB out of the cache in %u us", (unsigned)wide, (unsigned)all, (unsigned)window, (unsigned)(kept / 1024u),
             (unsigned)out);

#if DGX_DSI_HARDWARE_COPY
    if (bus->dma_pool) {
        /* the processor: many short rows and a few long ones tell a row from a byte */
        const uint32_t tall = dgx_dsi_bench_time(&bench, dgx_dsi_bench_copy, narrow, all, false);
        const uint32_t flat = dgx_dsi_bench_time(&bench, dgx_dsi_bench_copy, half, 16, false);
        const int64_t  window_byte = ((int64_t)window << 16) / (wide * all * bytes); /* a byte through the window, in 1/65536 us */
        int64_t        row, byte;
        dgx_dsi_bench_costs(tall, all, narrow * all * bytes, flat, 16, half * 16u * bytes, &row, &byte);
        /* the 2D DMA: a small one tells what asking it costs, two large ones what a byte does */
        const uint32_t small = dgx_dsi_bench_time(&bench, dgx_dsi_bench_dma, narrow, 8, false);
        const uint32_t some  = dgx_dsi_bench_time(&bench, dgx_dsi_bench_dma, wide, all / 4u, false);
        const uint32_t much  = dgx_dsi_bench_time(&bench, dgx_dsi_bench_dma, wide, all, false);
        ESP_LOGD(TAG, "bench: the processor copies %ux%u in %u us, %ux%u in %u us; the 2D DMA %ux8 in %u us, %ux%u in %u us, %ux%u in %u us", (unsigned)narrow,
                 (unsigned)all, (unsigned)tall, (unsigned)half, 16u, (unsigned)flat, (unsigned)narrow, (unsigned)small, (unsigned)wide, (unsigned)(all / 4u),
                 (unsigned)some, (unsigned)wide, (unsigned)all, (unsigned)much);
        if (small && some && much > some) {
            const int64_t dma_byte = ((int64_t)(much - some) << 16) / (wide * (all - all / 4u) * bytes);
            int64_t       asked    = (int64_t)small - ((dma_byte * narrow * 8u * bytes) >> 16);
            if (asked < 1) asked = 1;
            tuning->dma_kb_a_ms  = (uint16_t)dgx_dsi_bench_within((int64_t)wide * all * bytes * 1000 / 1024 / much, 1, 65535);
            tuning->dma_start_us = (uint16_t)dgx_dsi_bench_within(2 * asked, 100, 19999);
            /*
             * The processor takes rows * row + pixels * bytes * byte, the
             * DMA asked + pixels * bytes * dma_byte: the DMA is the faster
             * one from rows * (width + row / gain) = asked / gain on, where
             * gain is what a pixel costs the processor more. A fifth is
             * added: near the boundary neither is much faster. At 24 bits
             * it is doubled. There the piece is copied when the picture is
             * finished and the batch waits for it, and what was drawn a
             * moment ago is still in the cache, where the processor reads
             * it four times as fast as measured here. At 16 bits a piece
             * for the DMA is left for later, and the batch does not wait.
             */
            const int64_t gain = (byte - dma_byte) * bytes;
            if (gain > 0) {
                tuning->copy_row_px = (uint16_t)dgx_dsi_bench_within(row / gain, 0, 65535);
                tuning->copy_from   = dgx_dsi_bench_within((asked << 16) * (bytes == 2u ? 6 : 10) / 5 / gain, 1, UINT32_MAX);
            } else {
                tuning->copy_from = UINT32_MAX; /* the processor is as fast: nothing goes to the DMA */
            }
            /* rows of a program: the window against the DMA, and more room: what comes through a window stays in the cache, to be drawn over */
            tuning->rows_from = window_byte > dma_byte ? dgx_dsi_bench_within((asked << 16) * 8 / 5 / (window_byte - dma_byte), 1, UINT32_MAX) : UINT32_MAX;
        }
    }
#endif
#if DGX_DSI_HARDWARE_FILL
    if (bus->filler) {
        const uint32_t tall = dgx_dsi_bench_time(&bench, dgx_dsi_bench_fill, narrow / 2u, all, false);
        const uint32_t flat = dgx_dsi_bench_time(&bench, dgx_dsi_bench_fill, half, 16, false);
        int64_t        row, byte, fast_row, fast_byte;
        dgx_dsi_bench_costs(tall, all, narrow / 2u * all * bytes, flat, 16, half * 16u * bytes, &row, &byte);
        /* the accelerator: a small one, then a tall one and a wide one less the small one */
        const uint32_t small = dgx_dsi_bench_time(&bench, dgx_dsi_bench_accelerator, narrow / 2u, 16, false);
        const uint32_t thin  = dgx_dsi_bench_time(&bench, dgx_dsi_bench_accelerator, narrow / 2u, all, false);
        const uint32_t broad = dgx_dsi_bench_time(&bench, dgx_dsi_bench_accelerator, wide, all / 2u, false);
        ESP_LOGD(TAG, "bench: the processor fills %ux%u in %u us, %ux%u in %u us; the accelerator %ux16 in %u us, %ux%u in %u us, %ux%u in %u us",
                 (unsigned)(narrow / 2u), (unsigned)all, (unsigned)tall, (unsigned)half, 16u, (unsigned)flat, (unsigned)(narrow / 2u), (unsigned)small,
                 (unsigned)(narrow / 2u), (unsigned)all, (unsigned)thin, (unsigned)wide, (unsigned)(all / 2u), (unsigned)broad);
        if (small && thin && broad) {
            dgx_dsi_bench_costs(thin > small ? thin - small : 0, all - 16u, narrow / 2u * (all - 16u) * bytes, broad > small ? broad - small : 0, all / 2u - 16u,
                                (wide * (all / 2u) - narrow / 2u * 16u) * bytes, &fast_row, &fast_byte);
            int64_t asked = (int64_t)small - ((fast_row * 16 + fast_byte * (narrow / 2u * 16u * bytes)) >> 16);
            if (asked < 1) asked = 1;
            /*
             * As with the copy: the accelerator is the faster one from
             * rows + bytes / (row gain / byte gain) = asked / row gain on.
             * Two and a half times that is taken: what the accelerator has
             * filled is not in the cache, and what is drawn over it next
             * has to fetch it.
             */
            int64_t row_gain = row - fast_row;
            const int64_t byte_gain = byte - fast_byte;
            if (row_gain < 3277) row_gain = 3277; /* a twentieth of a microsecond: the sum has a row in it all the same */
            if (byte_gain > 0) {
                tuning->fill_div  = (uint16_t)dgx_dsi_bench_within(row_gain / byte_gain, 1, 65535);
                tuning->fill_from = (uint16_t)dgx_dsi_bench_within((asked << 16) * 5 / 2 / row_gain, 1, 65535);
                /* the least width and height together of a rectangle that passes, whichever of them runs along the rows */
                uint32_t sum = 65535;
                for (uint32_t rows = 1; rows < sum && rows < tuning->fill_from; ++rows) {
                    const uint64_t other = ((uint64_t)(tuning->fill_from - rows) * tuning->fill_div + rows * bytes - 1u) / (rows * bytes);
                    if (rows + other < sum) sum = (uint32_t)(rows + other);
                }
                if (tuning->fill_from < sum) sum = tuning->fill_from; /* rows alone pass: a strip one pixel wide */
                tuning->fill_sum = (uint16_t)sum;
            } else {
                tuning->fill_from = tuning->fill_sum = 65535; /* the processor is as fast: nothing goes to the accelerator */
            }
        }
    }
#endif
    heap_caps_free(bench.from);
    heap_caps_free(bench.to);
    heap_caps_free(bench.pixels);
    return true;
}


/*
 * Drawing functions of a screen on this bus. The pixels are memory at hand,
 * so a pixel and a rectangle go straight into it, past the window a display
 * behind a bus is drawn through; lines, circles and the rest are made of
 * these two.
 */

static inline dgx_dsi_bus_t *dgx_dsi_bus_of(dgx_screen_t *scr)
{
    return (dgx_dsi_bus_t *)((dgx_screen_with_bus_t *)scr)->bus;
}

static void dgx_dsi_scr_set_pixel(dgx_screen_t *scr, int x, int y, uint32_t color)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return;
    dgx_dsi_bus_t *bus = dgx_dsi_bus_of(scr);
    if (bus->frames == 2) {
        uint16_t px = (uint16_t)x, py = (uint16_t)y;
        dgx_dsi_draw_ready(bus);
        if (bus->window_open) dgx_dsi_window_close(bus);
        if (bus->turned) dgx_fb_area_point_to_panel(&bus->area, x, y, &px, &py);
        if (dgx_dsi_late_busy(bus)) dgx_dsi_late_clear(bus, px, py, px, py);
        dgx_fb_tiles_pixel(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), px, py);
    }
    dgx_fb_area_set_pixel(&bus->area, (uint16_t)x, (uint16_t)y, color);
    dgx_dsi_written(bus);
    dgx_screen_touch(scr, x, x, y, y);
}

static uint32_t dgx_dsi_scr_get_pixel(dgx_screen_t *scr, int x, int y)
{
    if (x < 0 || x >= scr->width || y < 0 || y >= scr->height) return 0;
    dgx_dsi_bus_t *bus = dgx_dsi_bus_of(scr);
    if (bus->frames == 2) {
        uint16_t px = (uint16_t)x, py = (uint16_t)y;
        if (bus->window_open) dgx_dsi_window_close(bus);
        if (bus->turned) dgx_fb_area_point_to_panel(&bus->area, x, y, &px, &py);
        /* a picture that is finished may still lack a piece left for later: it is brought as anything lacked is */
        dgx_fb_tiles_pixel_read(&bus->tiles, bus->area.fb, dgx_dsi_other(bus), px, py);
        if (bus->flip.told) dgx_dsi_read_out(bus);
    }
    return dgx_fb_area_get_pixel(&bus->area, (uint16_t)x, (uint16_t)y);
}

static void dgx_dsi_scr_fill_rectangle(dgx_screen_t *scr, int x, int y, int w, int h, uint32_t color)
{
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > scr->width) w = scr->width - x;
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > scr->height) h = scr->height - y;
    if (w <= 0 || h <= 0) return;
    dgx_dsi_bus_t *bus = dgx_dsi_bus_of(scr);
    if (bus->frames == 2) {
        dgx_dsi_draw_ready(bus);
        if (dgx_dsi_late_busy(bus)) {
            uint16_t x0, y0, x1, y1;
            dgx_fb_area_to_panel(&bus->area, x, x + w - 1, y, y + h - 1, &x0, &y0, &x1, &y1);
            dgx_dsi_late_clear(bus, x0, y0, x1, y1);
        }
        dgx_dsi_tiles_ready(bus, x, x + w - 1, y, y + h - 1, true, true);
    }
#if DGX_DSI_HARDWARE_FILL
    /*
     * Most rectangles are the short runs figures and letters are made of:
     * they are told apart by one sum, for no rectangle the accelerator takes
     * has less than 64 in it. Then the rows the rectangle takes on the
     * panel, its width when the axes are swapped, and its bytes.
     */
    if (w + h >= bus->tuning.fill_sum && bus->filler &&
        (uint32_t)(bus->area.swap_xy ? w : h) + (uint32_t)w * h * bus->area.pixel_bytes / bus->tuning.fill_div >= bus->tuning.fill_from &&
        dgx_dsi_hardware_fill(bus, x, y, w, h, color)) {
        bus->rows_written = true;
        dgx_screen_touch(scr, x, x + w - 1, y, y + h - 1);
        return;
    }
#endif
    dgx_fb_area_fill(&bus->area, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
    dgx_dsi_written(bus);
    dgx_screen_touch(scr, x, x + w - 1, y, y + h - 1);
}

/*
 * A rectangle of the screen moved to another place of it, for dgx_scroll():
 * the rows of the frame buffer are moved, each at once. With two frame
 * buffers the one drawing goes into gets what it lacks first, where the
 * picture is taken from and where it goes.
 */
static bool dgx_dsi_move(struct _dgx_bus_protocols_t *_bus, uint16_t left, uint16_t top, uint16_t width, uint16_t height, uint16_t to_left,
                         uint16_t to_top)
{
    dgx_dsi_bus_t *bus = (dgx_dsi_bus_t *)_bus;
    if (!bus->area.fb || !width || !height) return false;
    if (bus->frames == 2) {
        const int x0 = left < to_left ? left : to_left, x1 = (left < to_left ? to_left : left) + width - 1;
        const int y0 = top < to_top ? top : to_top, y1 = (top < to_top ? to_top : top) + height - 1;
        dgx_dsi_draw_ready(bus);
        if (dgx_dsi_late_busy(bus)) {
            uint16_t px0, py0, px1, py1;
            dgx_fb_area_to_panel(&bus->area, x0, x1, y0, y1, &px0, &py0, &px1, &py1);
            dgx_dsi_late_clear(bus, px0, py0, px1, py1);
        }
        dgx_dsi_tiles_ready(bus, x0, x1, y0, y1, false, true);
    }
    dgx_fb_area_move(&bus->area, left, top, width, height, to_left, to_top);
    dgx_dsi_written(bus);
    return true;
}

static void dgx_dsi_scr_update_screen(dgx_screen_t *scr, int left, int right, int top, int bottom)
{
    dgx_dsi_show(&dgx_dsi_bus_of(scr)->protocols);
}

void dgx_dsi_screen_functions(dgx_screen_t *scr)
{
    dgx_scr_init_slow_bus_optimized_funcs(scr);
    scr->set_pixel      = dgx_dsi_scr_set_pixel;
    scr->get_pixel      = dgx_dsi_scr_get_pixel;
    scr->fill_rectangle = dgx_dsi_scr_fill_rectangle;
    scr->update_screen  = dgx_dsi_scr_update_screen;
    /* the pixels are memory: a rectangle is moved in it */
    ((dgx_screen_with_bus_t *)scr)->bus->move = dgx_dsi_move;
}
