#pragma once
/*
 * dgx_host_screen.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  Displays on the computer: windows of SDL2 that show the glass of a panel,
 *  for writing an effect without flashing a board. The panel is a
 *  panel of the MIPI display command set behind a bus with a window, as the
 *  ILI9341 of the CYD is: the screen is made by dgx_panel_init(), and what is
 *  drawn on it goes the way it goes on the board, through the window, the
 *  buffer of the bus and its pieces. The bus puts the pixels into the glass
 *  in place of a wire.
 *
 *  What the computer does not tell: how long the drawing takes on the chip.
 *  What it tells instead, in the title of a window and when the program
 *  ends: how long the bytes sent would take on the wire of the board, from
 *  one wait of the program to the next, and how much memory the program and
 *  DGX hold. See "An effect on the computer" in README.md.
 *
 *  Built with host/dgx_host.mk, its group DGX_HOST_WINDOW, or by ESP-IDF
 *  for its linux target, never for a chip. A program tells the builds apart
 *  by DGX_HOST_WINDOW.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The board the host screen stands for, as far as its bus goes, and how it is held. */
typedef struct {
    /** The clock the bus of the board runs at, as spi_device_get_actual_freq() tells it there; 0 for 40 MHz, as on the CYD. */
    uint32_t    bus_hz;
    uint8_t     bus_lines;   /**< The data lines it sends pixels over: 1 for SPI, 4 for QSPI; 0 for 1. */
    const char *title;       /**< The title of the window; NULL for "DGX", "DGX 2" for the second one and so on. */
    uint32_t    buffer_max;  /**< As dgx_spi_options_t::buffer_max: the longest buffer of the bus; 0 for no limit. */
    /** What a transfer by the registers of the host costs above its bits, in us; 0 for 0.75, as on the CYD; below 0 for nothing. */
    float       transfer_us;
    /** What a piece of pixels by DMA costs to start, in us; 0 for 25, DGX_SPI_PIECE_COST_US; below 0 for nothing. */
    float       piece_us;
    /** The glass turned clockwise as the board is held, in degrees: 0, 90, 180 or 270. */
    uint16_t    turn;
    /**
     * The display of the board is on no bus the window stands for, as one on
     * MIPI-DSI is: the window counts no time on a wire, and the fields of the
     * bus above mean nothing.
     */
    bool        no_bus;
} dgx_host_screen_options_t;

/**
 * @brief The screen of a window on the computer, its glass width by height pixels.
 *
 * Every call makes a window of its own, with a bus of its own: a program for
 * a board with two displays has two windows. The windows are shown from the
 * first wait of the program on, vTaskDelay() or dgx_delay(), or once no
 * window has been made for 50 ms, and in its waits and while pixels go after
 * that; those made before are put side by side, each glass the same whole
 * number of times as large, and may be stretched. The panel scrolls itself
 * along the rows of the glass upright, as the ILI9341 does. Each window
 * counts what its own bus sends: two displays on one SPI host of the board
 * share its wire, and their times add up there. A closed window ends the
 * program. bus->dispose() of the screen's bus closes the window.
 *
 * @param width,height The glass, upright; dgx_panel_orientation() turns the screen on it as on a board.
 * @param color_bits   16 or 18.
 * @param options      NULL for the CYD: SPI at 40 MHz.
 * @return The screen, or NULL when there is no window.
 */
dgx_screen_t *dgx_host_screen_init(int width, int height, uint8_t color_bits, const dgx_host_screen_options_t *options);

/** @brief A finger on the glass of a window. */
typedef struct dgx_host_finger_t {
    int16_t x, y; /**< In pixels of the glass upright: before the turn of the window and before dgx_panel_orientation(). */
    uint8_t id;   /**< 0: the mouse. */
} dgx_host_finger_t;

/**
 * @brief The fingers on the glass of the window of a host screen, as the window last read its events.
 *
 * The mouse is a finger while its left button, pressed on this window, is
 * held, and while the mouse of DGX_HOST_SCRIPT is down on it; outside the
 * glass the finger lies on the edge nearest to it. The window reads its
 * events in the waits of the program and while pixels go, as it shows the
 * glass, and plays the script then. Any task may ask, while the screen
 * lives.
 *
 * @param scr     A screen of dgx_host_screen_init(); any other has no fingers.
 * @param fingers Where they are written.
 * @param max     How many fit there.
 * @return How many were written: 0 or 1, the mouse alone.
 */
int dgx_host_screen_fingers(const dgx_screen_t *scr, dgx_host_finger_t *fingers, int max);

/**
 * @brief The keys held down: those of the computer and those of DGX_HOST_SCRIPT.
 *
 * The keyboard is the program's, as a board has one set of buttons: a key
 * goes down while any window of the program has the keyboard, and is held
 * until it goes up, or until the keyboard leaves the windows of the
 * program. The windows read their events in the waits of the program and
 * while pixels go: a key pressed and let go of between two of them is
 * missed, as a button of the board read no more often would be. Any task
 * may ask.
 *
 * @param keys Where they are written, in ascending order: the numbers of the
 *             keys on the keyboard as USB HID has them, which SDL_Scancode
 *             is: 4 to 29 the letters A to Z, 40 Enter, 41 Escape, 44 the
 *             space bar, 79 to 82 the arrows right, left, down and up, 224 to
 *             231 Ctrl, Shift, Alt and GUI on the left, then on the right.
 * @param max  How many fit there.
 * @return How many were written.
 */
int dgx_host_keys(uint16_t *keys, int max);

#ifdef __cplusplus
}
#endif
