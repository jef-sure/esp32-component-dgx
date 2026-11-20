#pragma once
/*
 * dgx_rgb_esp32.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  A display on the parallel RGB interface of the chip (the LCD peripheral
 *  of an ESP32-S3, of an ESP32-P4): 16 data lines, a pixel clock, HSYNC,
 *  VSYNC and DE. The panel has no memory and no commands; the bus keeps its
 *  frame buffer in PSRAM and sends it all the time, through two bounce
 *  buffers in internal memory that the bus fills from it. Drawing goes into
 *  the frame buffer and is on the glass at the next pass of the scan.
 *
 *  The bus is made with the pins of the board and the pixel clock, which is
 *  the board's: what its PSRAM and its wires carry. The panel driver gives
 *  the size and the timing of the glass, see drivers/rgb_panel.h.
 *
 *  The copy into the bounce buffers runs in an interrupt on the core that
 *  made the display: on an ESP32-S3 at 33 MHz and 800x480 it takes an
 *  eighth of that core while the picture stands and a third while the
 *  program draws, the rows of each copy brought into the cache before it.
 *  Make the display on the core that draws: the interrupt then stops the
 *  drawing while it copies, and the copy has PSRAM to itself. Made on the
 *  other core, on a Sunton ESP32-8048S070 at 33 MHz, the copy ran beside
 *  the drawing, could not keep up, and the picture shook. At 33 MHz the
 *  other core keeps off PSRAM; with drawing there the bus holds 20 MHz.
 *
 *  A program that writes to the flash, to NVS or for an update, sets
 *  CONFIG_SPIRAM_XIP_FROM_PSRAM. Without it IDF turns the cache off for
 *  every write to the flash, the copy cannot read the frame buffer
 *  meanwhile, and the picture is spoilt. DGX does not set it: it is a
 *  setting of the whole program, and the code and the constants of the
 *  program take their size again in PSRAM. See docs/rgb-display-en.md.
 *
 *  CONFIG_LCD_RGB_RESTART_IN_VSYNC of IDF is best left off. With it, up to
 *  IDF 5.5.6, 6.0.3 and 6.1, esp_lcd starts every frame from the first
 *  bounce buffer but goes on counting which buffer to fill as before: after
 *  one copy more than a period late it fills the buffer being sent from then
 *  on, and the picture keeps short stripes until the display is made again.
 *  IDF 6.1.1 counts anew at every frame, as esp_lcd without the option
 *  always does.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "driver/gpio.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/** @brief The pins of a parallel RGB display. */
typedef struct _dgx_rgb_pins_t {
    gpio_num_t pclk;     /*!< pixel clock */
    gpio_num_t hsync;    /*!< horizontal sync */
    gpio_num_t vsync;    /*!< vertical sync */
    gpio_num_t de;       /*!< data enable; GPIO_NUM_NC for a panel taking the syncs only */
    gpio_num_t disp;     /*!< the display enable line of the panel, high for on; GPIO_NUM_NC when it has none */
    gpio_num_t data[16]; /*!< D0..D15 of RGB565: blue from its lowest bit, then green, then red up to its highest */
} dgx_rgb_pins_t;

/**
 * @brief Make a bus for a parallel RGB display.
 *
 * Nothing is sent until a panel driver makes the display on it, as
 * dgx_rgb_panel_init() does.
 *
 * @param pins    The pins of the board.
 * @param pclk_hz The pixel clock: 33 MHz for the 800x480 glass of a Sunton
 *                ESP32-8048S070, 60 frames a second, which the board holds
 *                while the processor draws as fast as it can.
 * @return The bus, or NULL on failure. Give it up with dgx_bus_dispose() once its screen is destroyed.
 */
dgx_bus_protocols_t *dgx_rgb_init(const dgx_rgb_pins_t *pins, uint32_t pclk_hz);

/**
 * @brief What the copies into the bounce buffers cost, counted since the bus started.
 *
 * Look at the numbers a second apart: copy_us against the second is how
 * much of the core that made the display the copies take, and against
 * copies x period_us how much of its time a copy takes. A gap_us_most
 * longer than two periods means a copy began after the panel had started
 * to send its bounce buffer, which still held rows of before: the bus did
 * not keep up. Each count goes round after 2^32; the two longest times are
 * of the time since the counters were last read, and start again.
 */
typedef struct _dgx_rgb_counters_t {
    uint32_t frames;       /*!< frames started */
    uint32_t copies;       /*!< bounce buffers filled */
    uint32_t copy_us;      /*!< the time of all copies, in microseconds */
    uint32_t copy_us_most; /*!< the longest copy */
    uint32_t period_us;    /*!< what the panel takes to send one bounce buffer, the time a copy has */
    uint32_t gap_us_most;  /*!< the longest time between the starts of two copies, which come period_us apart: a copy started late by the difference */
} dgx_rgb_counters_t;

/** @brief Read the counters of the display on this bus. */
void dgx_rgb_counters(dgx_bus_protocols_t *bus, dgx_rgb_counters_t *counters);

/** @brief How the bus did at one pixel clock of dgx_rgb_bench(). */
typedef struct _dgx_rgb_bench_step_t {
    uint32_t pclk_hz;     /*!< the clock tried, as asked */
    uint16_t frames_x10;  /*!< frames a second it gave, ten times */
    uint16_t period_us;   /*!< the time a copy has */
    uint16_t gap_us;      /*!< the longest time between two copies, with the picture still or while drawing */
    uint8_t  still_pct;   /*!< the share of the core of the display the copies took with nothing drawn */
    uint8_t  drawing_pct; /*!< and while the bench drew */
    uint8_t  holds;       /*!< 1: the bus kept up, gap_us within two periods */
    uint8_t  reserved[3];
} dgx_rgb_bench_step_t;

#define DGX_RGB_BENCH_VERSION 1
/** @brief The clocks dgx_rgb_bench() tries at most. */
#define DGX_RGB_BENCH_STEPS 8

/** @brief dgx_rgb_bench_t.flags: the display was made on another core than the one the bench drew from. */
#define DGX_RGB_BENCH_OTHER_CORE 0x01u
/** @brief dgx_rgb_bench_t.flags: CONFIG_SPIRAM_XIP_FROM_PSRAM is off, and writing to the flash spoils the picture. */
#define DGX_RGB_BENCH_NO_XIP 0x02u
/** @brief dgx_rgb_bench_t.flags: CONFIG_LCD_RGB_RESTART_IN_VSYNC is set with an IDF whose esp_lcd keeps on counting across frames. */
#define DGX_RGB_BENCH_RESTART 0x04u

/**
 * @brief What dgx_rgb_bench() found on the board at hand.
 *
 * The structure is flat and holds no pointers: a program may keep it, in
 * NVS for one, and ask dgx_rgb_advise() at a later start instead of
 * measuring again.
 */
typedef struct _dgx_rgb_bench_t {
    uint16_t             version;      /*!< DGX_RGB_BENCH_VERSION */
    uint16_t             size;         /*!< sizeof(dgx_rgb_bench_t) */
    uint16_t             width;        /*!< the panel: pixels in a row */
    uint16_t             height;       /*!< and rows */
    uint32_t             pclk_hz;      /*!< the clock the bus ran at, and runs at again after the bench */
    uint32_t             sheet_min_hz; /*!< the clocks the sheet of the glass allows; 0 when not known */
    uint32_t             sheet_max_hz;
    uint8_t              steps;        /*!< of `step`, filled from the highest clock down: the last is the one that held, unless none did */
    uint8_t              display_core; /*!< the core the display was made on */
    uint8_t              drawing_core; /*!< the core the bench drew from: the program's */
    uint8_t              flags;        /*!< DGX_RGB_BENCH_* */
    dgx_rgb_bench_step_t step[DGX_RGB_BENCH_STEPS];
} dgx_rgb_bench_t;

/**
 * @brief Find the highest pixel clock at which the bus keeps up on the board at hand.
 *
 * From the top of what the glass allows, or from the clock of the bus when
 * that is not known, down by an eighth at a time, at most
 * DGX_RGB_BENCH_STEPS clocks, until one holds. At each: half a second with
 * nothing drawn, then a second of filling and copying as fast as it can in
 * PSRAM of its own, two buffers of a third of the panel, from the core it is
 * called on. Call it from the task that draws, while whatever else the
 * program runs goes on as usual, on either core: that is measured too. The
 * picture on the screen does not change, and nothing may be drawn on it
 * meanwhile. A clock holds when no copy started after the panel had begun to
 * send its bounce buffer: the longest time between two copies, see
 * dgx_rgb_counters(), is within two periods. What the bench found is logged.
 * Takes 1.5 s a clock, so it is for once, not for every start: keep what it
 * gives.
 *
 * The bus runs at its own clock again at the end: give the one
 * dgx_rgb_advise() names to dgx_rgb_init() at the next start.
 *
 * @return false when there is no display yet, or no memory for the bench.
 */
bool dgx_rgb_bench(dgx_bus_protocols_t *bus, dgx_rgb_bench_t *bench);

/**
 * @brief The pixel clock to give dgx_rgb_init(): the highest that held in dgx_rgb_bench(); 0 when none did.
 *
 * Measures nothing, so a program can ask before it makes the display, with
 * what it kept in NVS. What else the bench found is for the developer, the
 * core of the display and the settings of the build: dgx_rgb_bench_text().
 * One frame buffer or two is not advised: one does not make the bus fall
 * behind, it only lets a moving picture tear, and that depends on what the
 * program draws. The clock holds with either.
 */
uint32_t dgx_rgb_advise(const dgx_rgb_bench_t *bench);

/**
 * @brief What dgx_rgb_bench() found, in words, for whoever reads them: a log, a screen, a bug report.
 *
 * The clock that held and what the copies took there, the clock above it
 * that did not, and what mends it: the display made on the core that draws,
 * the other core kept off PSRAM. Then the settings of the build that spoil
 * the picture: CONFIG_SPIRAM_XIP_FROM_PSRAM off, CONFIG_LCD_RGB_RESTART_IN_VSYNC
 * on with an IDF that keeps on counting. English, under 700 characters.
 *
 * @param text Where to write; always a string, cut when `size` is small.
 * @return The length of the whole text, as snprintf(): `size` or more when it was cut.
 */
int dgx_rgb_bench_text(const dgx_rgb_bench_t *bench, char *text, size_t size);

/*
 * The rest is for panel drivers.
 */

/** @brief The picture a panel takes: its size, the timing of a frame and how its lines are driven. */
typedef struct _dgx_rgb_video_t {
    uint16_t width;             /*!< pixels in a row of the panel */
    uint16_t height;            /*!< rows of the panel */
    uint16_t hsync_pulse_width; /*!< horizontal sync, in pixel clocks */
    uint16_t hsync_back_porch;
    uint16_t hsync_front_porch;
    uint16_t vsync_pulse_width; /*!< vertical sync, in rows */
    uint16_t vsync_back_porch;
    uint16_t vsync_front_porch;
    bool     hsync_idle_low;  /*!< HSYNC is low between its pulses */
    bool     vsync_idle_low;  /*!< VSYNC is low between its pulses */
    bool     de_idle_high;    /*!< DE is high while no pixel is sent */
    bool     pclk_active_neg; /*!< the panel takes the data on the falling edge of the pixel clock */
} dgx_rgb_video_t;

/**
 * @brief Make the frame buffers of a display and start sending them; they are black.
 *
 * Called by a panel driver once.
 *
 * @param bus     Bus from dgx_rgb_init().
 * @param video   Size and timing of the picture.
 * @param swap_rb The panel takes red and blue the other way round.
 * @param frames  1, or 2: drawing goes into the one the panel is not sent, and a batch is shown whole.
 * @return false on failure; the bus then takes a display again.
 */
bool dgx_rgb_video_setup(dgx_bus_protocols_t *bus, const dgx_rgb_video_t *video, bool swap_rb, uint8_t frames);

/** @brief The pixel clock the bus was made with. */
uint32_t dgx_rgb_pixel_clock(dgx_bus_protocols_t *bus);

/**
 * @brief Set how the screen lies on the panel, as the MX, MY and MV bits of a display controller do.
 *
 * It is done by the bus where pixels are put. What the frame buffer holds
 * stays as it is. The window becomes the whole screen.
 */
void dgx_rgb_orientation(dgx_bus_protocols_t *bus, bool mirror_x, bool mirror_y, bool swap_xy);

/** @brief The pixel clocks the sheet of the glass allows, for dgx_rgb_bench() to start from. */
void dgx_rgb_clock_range(dgx_bus_protocols_t *bus, uint32_t min_hz, uint32_t max_hz);

/** @brief Give a screen on this bus the drawing functions of a screen whose pixels are memory. */
void dgx_rgb_screen_functions(dgx_screen_t *scr);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
