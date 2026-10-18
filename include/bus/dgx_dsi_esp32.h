#pragma once
/*
 * dgx_dsi_esp32.h
 *
 *  Created on: 11.10.2026
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief Initialize a MIPI-DSI bus backend.
 *
 * A display on this bus is refreshed all the time from a frame buffer in
 * PSRAM. For a panel driver the bus looks like a display with memory of its
 * own: it takes the window and the pixels that follow and puts them into the
 * frame buffer, in the byte order that buffer has. Other commands go to the
 * display as they are.
 *
 * @param bus_id             DSI controller, 0 on ESP32-P4.
 * @param data_lanes         Number of data lanes the display is wired with.
 * @param lane_bit_rate_mbps Bit rate of a lane in Mbit/s, as the display needs.
 * @param phy_ldo_chan       Channel of the chip's regulator that powers the DSI PHY
 *                           (3 on the usual ESP32-P4 boards), -1 when the PHY is powered otherwise.
 * @param phy_ldo_mv         Voltage of that channel in mV (2500 on the usual boards).
 * @return Bus backend usable by panel drivers, or NULL on failure.
 */
dgx_bus_protocols_t *dgx_dsi_init(int bus_id, uint8_t data_lanes, uint32_t lane_bit_rate_mbps, int phy_ldo_chan, int phy_ldo_mv);

/**
 * @brief What a display on this bus may spend to show a picture whole; given to the constructor of a display.
 *
 * The display is sent a frame buffer all the time, while it is being drawn
 * into as well. There is no way that suits every program, so the program
 * chooses.
 */
typedef enum {
    /**
     * One frame buffer, and drawing goes into it. No memory beyond the one
     * frame, and what is drawn shows at once. A picture that changes over
     * much of the screen may be seen half updated for a frame (tearing): the
     * more the longer it takes to draw against the 1/60 s of a frame.
     */
    DgxDsiOneFrame = 1,
    /**
     * One frame buffer and no memory beyond it; what is put to the display
     * waits for the scan of the display to be out of the way. A compromise:
     * a picture that comes from a virtual screen is seen whole, drawing on
     * the display itself only in part.
     *
     * A part of a virtual screen that goes by the 2D DMA and a large
     * rectangle filled by the accelerator are started when the scan is not
     * in their rows. Both write faster than the display reads and stay
     * ahead of the scan, so the part or the rectangle is seen whole, the
     * whole screen as well.
     *
     * Pixels the processor sends through the window (small parts, a turned
     * screen, 24-bit color, text) wait until the scan has left the rows of
     * the window, when they can be written before it is back. That holds
     * for about 160 rows of 1024 pixels; a larger window goes as it comes,
     * as with DgxDsiOneFrame. What a batch has left in the cache of the
     * chip is brought out the same way at its end.
     *
     * Single pixels, lines and small rectangles drawn on the display itself
     * are not held back. And a batch is not shown as one: of many tall
     * rectangles some are seen a frame later than the others. What has to
     * appear at once is drawn in a virtual screen and put to the display.
     *
     * The wait does not set the pace: it is over at once when the scan is
     * elsewhere, so a program may update more than once a frame.
     */
    DgxDsiOneFrameAfterScan = 3,
    /**
     * Two frame buffers. Drawing goes into the one the display is not
     * reading, and a batch of drawing is shown whole, from the next frame
     * on. Costs one more frame of PSRAM (1.2 MB for 1024x600 at 16 bits)
     * and 2.2 KB of RAM. What was drawn is given to the other frame buffer
     * afterwards, by tiles of 32x4 pixels: only the tiles that were drawn
     * in, and only what the next batch does not write over whole. A large
     * piece is given after the display has taken the picture, by the 2D
     * DMA ahead of its scan, at 16 bits a pixel, so what is drawn over it
     * meanwhile is not copied at all.
     *
     * The end of a batch does not show it at once: it tells that there is
     * a picture to show, and the display takes it where its frame ends.
     * Until then the batches that follow go into the same picture, so what
     * is drawn in the time of a frame, by one task or by several, is shown
     * together, by one change of the frame buffers. Only a batch that
     * comes when the frame is about to end waits for its end, for a batch
     * being drawn where a frame ends would keep the whole picture back a
     * frame.
     *
     * The display is not a pace-maker: a program that draws more often
     * than frames are shown gets all it draws into the pictures and sees
     * the last of it, and has to hold itself back. Which of several tasks
     * gets how much of the screen's time is the program's business too.
     *
     * Put what belongs to one picture between dgx_screen_progress_up() and
     * dgx_screen_progress_down(): a primitive drawn outside a batch is a
     * batch of its own, and may be shown without the next one. Tasks that
     * draw on one screen still have to do it one at a time, as on any
     * screen.
     */
    DgxDsiTwoFrames = 2,
} dgx_dsi_frames_t;

/**
 * @brief What a display on this bus has done since it was started.
 *
 * To see what a program costs the display, and not how many pictures the
 * program has made: look at the numbers a second apart. Every one of them
 * goes round after 2^32.
 */
typedef struct _dgx_dsi_counters_t {
    uint32_t frames;       /*!< frames sent to the display; counted with two frame buffers and behind the scan, 0 otherwise */
    uint32_t batches;      /*!< batches of drawing that ended with something drawn */
    uint32_t flips;        /*!< two frame buffers: times they changed places, that is frames that showed something new */
    uint32_t joined;       /*!< batches that went into a picture already finished and were shown together with it */
    uint32_t kept_back;    /*!< ends of frames a finished picture was kept back over, a batch being drawn into it */
    uint32_t flip_waits;   /*!< times a batch waited for the display to take a picture */
    uint32_t flip_wait_us; /*!< and how long in all, in microseconds */
    uint32_t scan_waits;   /*!< behind the scan: times a write waited for the scan to be out of its rows */
    uint32_t scan_wait_us; /*!< and how long in all */
    uint32_t copied_bytes; /*!< two frame buffers: bytes copied from one into the other */
    uint32_t copied_late_bytes;  /*!< of them, copied after the display had taken the picture, ahead of its scan */
    uint32_t copied_late_behind; /*!< pieces so copied that were there only after the display had read past them */
    uint32_t copy_waits;   /*!< times drawing waited for such a piece to be copied */
    uint32_t copy_wait_us; /*!< and how long in all */
    uint32_t copy_kb_a_ms; /*!< not a count: KB the 2D DMA copies in a millisecond, as the bus has measured it; 0 until it has */
} dgx_dsi_counters_t;

/**
 * @brief Read the counters of a display on this bus.
 * @param bus      Bus from dgx_dsi_init().
 * @param counters Out: the numbers as they are now.
 */
void dgx_dsi_counters(dgx_bus_protocols_t *bus, dgx_dsi_counters_t *counters);

/**
 * @brief How long the display takes for a frame, in microseconds.
 *
 * It shows no more pictures a second than 1000000 by this. A program that
 * wants that many holds itself back: the bus does not wait for a frame,
 * and what is drawn in time is shown in the next one. How to hold back so
 * that pictures come evenly see at dgx_dsi_frame_ended().
 *
 * Reckoned from the timings of the panel at first. With two frame buffers
 * and behind the scan the bus counts frames, and half a second
 * later the number is the time of the frames counted, 256 of them at a
 * time: the clock of the display is not exactly the one that was asked.
 *
 * @param bus Bus from dgx_dsi_init(), with its display made.
 */
uint32_t dgx_dsi_frame_us(dgx_bus_protocols_t *bus);

/**
 * @brief When the last frame of the display ended.
 *
 * A program that keeps the pace of the display by its own clock alone does
 * not know where in a frame it is. A picture finished near the end of a
 * frame gets now into this frame, now into the next, and moves unevenly.
 * With this a program aims a picture: it starts it so as to be finished a
 * little before a frame ends, by how long its last pictures took. The bus
 * does not wait and holds nobody back: it tells the time it writes down
 * where a frame ends, and the program sleeps as it likes.
 *
 * @code
 * int64_t ended;
 * if (dgx_dsi_frame_ended(bus, &ended)) {
 *     // to be finished 4 ms before the next frame ends
 *     int64_t from = ended + dgx_dsi_frame_us(bus) - 4000 - picture_takes_us;
 *     while (esp_timer_get_time() < from) vTaskDelay(1);
 * }
 * @endcode
 *
 * demo_pace() of examples/dsi_demo does it in full. Starting right after a
 * frame has ended is not the same: a picture that takes about a frame is
 * then finished at the end of one. Measured on an ESP32-P4 with one task
 * that draws a picture every second frame: aimed so, 302 of 303 pictures
 * came two frames apart; started after a frame end, half of them did; by
 * the clock alone it was chance, all of them in one run and half in
 * another.
 *
 * @param bus      Bus from dgx_dsi_init(), with its display made.
 * @param ended_us Out: the time, as esp_timer_get_time() gives it.
 * @return false when the bus does not know: with DgxDsiOneFrame it does not count frames, and it does not know
 *         before the first frame has ended or after the display has been stopped.
 */
bool dgx_dsi_frame_ended(dgx_bus_protocols_t *bus, int64_t *ended_us);

/**
 * @brief Optional: the size of the tiles of a display with two frame buffers.
 *
 * With DgxDsiTwoFrames the bus keeps by tiles what one frame buffer lacks
 * of the other, a bit a tile, and copies a tile when it is first drawn in.
 * Small tiles copy less for pixels set here and there, large ones cost
 * less for lines, bars and fills, and the maps take 8 bytes of RAM a row of
 * tiles. Without this call the tiles are DGX_DSI_TILE_WIDTH by
 * DGX_DSI_TILE_HEIGHT pixels.
 *
 * Before the constructor of the display the size is only remembered. After
 * it, between two batches of drawing and not inside one, the bus waits
 * until the display has taken the picture that is finished and makes the
 * maps anew. With one frame buffer there are no tiles, and the size is
 * remembered for nothing.
 *
 * Choose the size once, for what the program draws, and give it before the
 * constructor. It is not for changing as the program goes: a change makes
 * the drawing task wait for the display, 11 ms on average and up to 23 ms
 * on an ESP32-P4, the picture stands still meanwhile, the maps are taken
 * from the heap anew every time, and what a tile that suits gains is some
 * percent. The call after the constructor is for a program that learns the
 * size late, from its settings for instance.
 *
 * @param bus    Bus from dgx_dsi_init().
 * @param width  Pixels, a power of two from DGX_DSI_TILE_WIDTH_LEAST to DGX_DSI_TILE_WIDTH_MOST; the bus makes the tiles wider
 *               when a row of the panel is more than 32 of them.
 * @param height Rows, a power of two from 1 to DGX_DSI_TILE_HEIGHT_MOST.
 * @return ESP_OK when the size is taken. Otherwise the tiles stay as they were, with a warning in the log:
 *         ESP_ERR_INVALID_ARG for no bus or a size that is not such powers of two, ESP_ERR_NO_MEM when there is
 *         no memory for the maps, ESP_ERR_INVALID_STATE when a batch of drawing is under way.
 */
esp_err_t dgx_dsi_tiles(dgx_bus_protocols_t *bus, uint16_t width, uint16_t height);

/** @brief The tiles a display with two frame buffers has unless dgx_dsi_tiles() gives another size. */
#define DGX_DSI_TILE_WIDTH  32
#define DGX_DSI_TILE_HEIGHT 4
/** @brief The sizes dgx_dsi_tiles() takes: powers of two within these. */
#define DGX_DSI_TILE_WIDTH_LEAST 8
#define DGX_DSI_TILE_WIDTH_MOST  1024
#define DGX_DSI_TILE_HEIGHT_MOST 128

/** @brief The form of dgx_dsi_tuning_t: numbers stored in another form are not taken. */
#define DGX_DSI_TUNING_VERSION 3

/**
 * @brief The numbers a DSI bus decides by.
 *
 * When a piece of a picture goes by the 2D DMA or the accelerator and when
 * by the processor, how fast pixels get into the frame buffer and out of
 * the cache, how much room is left before the scan of the display: all of
 * that depends on the chip, its memory and the display. A bus starts with
 * the numbers of the board it was written on, an ESP32-P4 of revision 1.3
 * with PSRAM at 200 MHz and a panel of 1024x600. dgx_dsi_bench() measures
 * them on the board at hand, dgx_dsi_tune() gives them to the bus.
 *
 * The structure is flat and holds no pointers: a program may keep it, in
 * NVS for one, and give it to the bus at every start instead of measuring
 * again.
 */
typedef struct _dgx_dsi_tuning_t {
    uint16_t version;        /*!< DGX_DSI_TUNING_VERSION */
    uint16_t size;           /*!< sizeof(dgx_dsi_tuning_t) */
    uint16_t width;          /*!< the panel the numbers are for: pixels in a row */
    uint16_t height;         /*!< and rows */
    uint8_t  pixel_bytes;    /*!< and bytes a pixel */
    uint8_t  scan_lead;      /*!< rows the display reads ahead of the row it shows; not measured */
    uint8_t  shared_halves;  /*!< a copy by the 2D DMA takes this many halves of its time while another transfer goes on; not measured */
    uint8_t  lanes_percent;  /*!< how much of what the DSI lanes carry the pixels of the panel take, by the timings; 0: not known */
    uint16_t window_kb_a_ms; /*!< KB the processor puts through a window in a millisecond, with room */
    uint16_t out_kb_a_ms;    /*!< KB the cache brings out in a millisecond, with room */
    uint16_t dma_kb_a_ms;    /*!< KB the 2D DMA copies in a millisecond; 0: not known, the bus finds it out as it goes */
    uint16_t dma_start_us;   /*!< room for starting a transfer by the 2D DMA */
    uint16_t scan_room_us;   /*!< room added to what a write behind the scan takes; not measured */
    uint16_t guard_us;       /*!< how close to the end of a frame a word to the display is not relied on; not measured */
    uint16_t copy_row_px;    /*!< a rectangle is copied by the 2D DMA when rows * (width + copy_row_px) >= copy_from */
    uint16_t fill_sum;       /*!< a rectangle is filled by the accelerator when width + height >= fill_sum ... */
    uint16_t fill_div;       /*!< ... and rows + bytes / fill_div >= fill_from */
    uint16_t fill_from;
    uint32_t copy_from;
    uint32_t rows_from;      /*!< rows of pixels of a program go by the 2D DMA from this many bytes on */
    uint16_t fifo_drops;     /*!< while the 2D DMA copies as fast as it can: times a frame the display runs short of data; DGX_DSI_NOT_MEASURED: not measured */
    uint16_t psram_mhz;      /*!< the clock of the PSRAM the numbers were measured at, CONFIG_SPIRAM_SPEED; 0 without PSRAM */
    uint16_t chip_revision;  /*!< of the chip, as esp_chip_info() gives it: 302 for v3.2; a fact for whoever reads the numbers */
    uint16_t reserved;
} dgx_dsi_tuning_t;

/** @brief dgx_dsi_tuning_t.fifo_drops of a bus with no 2D DMA, or before dgx_dsi_bench(). */
#define DGX_DSI_NOT_MEASURED 0xffffu

/**
 * @brief Read the numbers the bus decides by now.
 * @param bus Bus from dgx_dsi_init(), with its display made.
 */
void dgx_dsi_tuning(dgx_bus_protocols_t *bus, dgx_dsi_tuning_t *tuning);

/**
 * @brief Give the bus numbers to decide by: those dgx_dsi_bench() measured, now or at an earlier start.
 * @return false when they are not taken: another form, another panel or color depth, or a number that makes no sense.
 */
bool dgx_dsi_tune(dgx_bus_protocols_t *bus, const dgx_dsi_tuning_t *tuning);

/**
 * @brief Measure the numbers on the board at hand.
 *
 * Works in memory of its own, two pictures of 128 rows of the panel, half a
 * megabyte for 1024 pixels in a row at 16 bits, given back at the end:
 * nothing on the screen changes. The display has to be running, for it
 * takes its share of the memory all the time and the numbers are to be
 * those of a display at work, and nothing may be drawn meanwhile. Last, the
 * 2D DMA copies for a dozen frames as fast as it can while the bus watches
 * how often the display runs short of data (`fifo_drops`), for
 * dgx_dsi_advise() and dgx_dsi_tuning_text(). What the numbers say about
 * the board is logged, as a warning when it has little margin. Takes about
 * 0.3 s, so it is for once, not for every start: keep what it gives.
 *
 * The bus goes on with the numbers it had: give it the new ones by
 * dgx_dsi_tune().
 *
 * @param tuning Out: the numbers measured, the rest as the bus has them.
 * @return false when there is no display yet or no memory.
 */
bool dgx_dsi_bench(dgx_bus_protocols_t *bus, dgx_dsi_tuning_t *tuning);

/** @brief dgx_dsi_advise(): with one frame buffer heavy drawing leaves the display short of data, and the screen flashes for a frame. */
#define DGX_DSI_ADVISE_TWO_FRAMES 0x01u

/**
 * @brief What the numbers measured tell a program to do: the one choice a program makes at run time.
 *
 * Measures nothing: it reads the numbers dgx_dsi_bench() gave, now or at an
 * earlier start, so a program can ask before it makes the display, with the
 * numbers it kept in NVS. When the bridge in front of the DSI host runs out
 * of data, it sends a color of its own for the rest of the frame; the
 * screen flashes for a frame, light blue at 16 bits, blue at 24.
 *
 * - DGX_DSI_ADVISE_TWO_FRAMES: give the display DgxDsiTwoFrames. A screen
 *   that never draws much at once may do without.
 *
 * What else the numbers say is for the developer, not the program: the
 * color depth, the PSRAM clock and the lanes are settings of the build and
 * the board. dgx_dsi_tuning_text() puts it in words, and dgx_dsi_bench()
 * logs those words. The limit was found by eye on one board, see
 * docs/dsi-usage-en.md.
 *
 * @return DGX_DSI_ADVISE_* flags; 0 when one frame buffer will do.
 */
uint8_t dgx_dsi_advise(const dgx_dsi_tuning_t *tuning);

/**
 * @brief What the numbers measured say about the board, in words, for whoever reads them: a log, a screen, a bug report.
 *
 * The PSRAM clock and the chip revision; how often the display ran short of
 * data under the 2D DMA and whether that is margin enough, and if not what
 * mends it (two frame buffers, or a faster PSRAM clock if the chip has
 * one); how much of what the DSI lanes carry the pixels take and whether
 * that leaves room. A library that cannot act on this hands the text on to
 * its user. English, one paragraph, under 400 characters.
 *
 * @param text Where to write; always a string, cut when `size` is small.
 * @return The length of the whole text, as snprintf(): `size` or more when it was cut.
 */
int dgx_dsi_tuning_text(const dgx_dsi_tuning_t *tuning, char *text, size_t size);

/*
 * The rest is for panel drivers.
 */

/** @brief The picture a display takes: its size and the timing of a frame. */
typedef struct _dgx_dsi_video_t {
    uint16_t width;             /*!< pixels in a row of the panel */
    uint16_t height;            /*!< rows of the panel */
    uint32_t pixel_clock_khz;   /*!< pixel clock in kHz */
    uint16_t hsync_pulse_width; /*!< horizontal sync, in pixels */
    uint16_t hsync_back_porch;
    uint16_t hsync_front_porch;
    uint16_t vsync_pulse_width; /*!< vertical sync, in rows */
    uint16_t vsync_back_porch;
    uint16_t vsync_front_porch;
} dgx_dsi_video_t;

/**
 * @brief Make the frame buffer of a display; it is black and not shown yet.
 *
 * Called by a panel driver once, before it sends its commands.
 *
 * @param bus        Bus from dgx_dsi_init().
 * @param video      Size and timing of the picture.
 * @param color_bits 16 or 24.
 * @param swap_rb    The display takes red and blue the other way round.
 * @param frames     The way to update the display the program asked its constructor for.
 * @return false on failure.
 */
bool dgx_dsi_video_setup(dgx_bus_protocols_t *bus, const dgx_dsi_video_t *video, uint8_t color_bits, bool swap_rb, dgx_dsi_frames_t frames);

/**
 * @brief Start sending the frame buffer to the display, after its commands.
 * @return false on failure.
 */
bool dgx_dsi_video_start(dgx_bus_protocols_t *bus);

/**
 * @brief Set how the screen lies on the panel, as the MX, MY and MV bits of a display controller do.
 *
 * It is done by the bus where pixels are put, so it works with any display.
 * What the frame buffer holds stays as it is. The window becomes the whole
 * screen.
 *
 * @param mirror_x Columns of the panel are counted from the right.
 * @param mirror_y Rows of the panel are counted from the bottom.
 * @param swap_xy  x of the screen runs along the rows of the panel.
 */
void dgx_dsi_orientation(dgx_bus_protocols_t *bus, bool mirror_x, bool mirror_y, bool swap_xy);

/**
 * @brief Give a screen on this bus its drawing functions; called by a panel driver last, when the display is set up.
 *
 * The functions are those of any display behind a bus, but a pixel and a
 * rectangle are put straight into the frame buffer and a pixel is read
 * back from it, as a virtual screen does with its memory. update_screen()
 * brings out what was drawn, see dgx_dsi_show().
 *
 * @param scr A dgx_screen_with_bus_t whose bus is this one, with the size and the color depth set.
 */
void dgx_dsi_screen_functions(dgx_screen_t *scr);

/**
 * @brief Make what was written to the frame buffer since the last call seen by the display.
 *
 * Pixels are written through the cache of the chip, and the display is sent
 * the memory behind it. The update_screen() of a screen calls this, so
 * outside a batch every primitive is seen at once, and a batch is brought
 * out at its end: pixels set one by one cost several times less that way.
 * The bus also does it on its own when much has been drawn by pixels and
 * rectangles, so a long batch shows in the order it is drawn. With two
 * frame buffers this is where the display is told that there is a picture
 * to show; it takes it where its frame ends.
 */
void dgx_dsi_show(dgx_bus_protocols_t *bus);

/** @brief Number of data lanes the bus was made with. */
uint8_t dgx_dsi_data_lanes(dgx_bus_protocols_t *bus);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
