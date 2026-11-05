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
 *  Built with host/dgx_host.mk, its group DGX_HOST_WINDOW; never under
 *  ESP-IDF. A program tells the two builds apart by DGX_HOST_WINDOW.
 */

#include <stdint.h>

#include "dgx_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The board the host screen stands for, as far as its bus goes. */
typedef struct {
    uint32_t    bus_hz;    /**< The clock of the bus of the board; 0 for 40 MHz, as on the CYD. */
    uint8_t     bus_lines; /**< The data lines it sends over: 1 for SPI, 4 for QSPI; 0 for 1. */
    const char *title;     /**< The title of the window; NULL for "DGX", "DGX 2" for the second one and so on. */
} dgx_host_screen_options_t;

/**
 * @brief The screen of a window on the computer, its glass width by height pixels.
 *
 * Every call makes a window of its own, with a bus of its own: a program for
 * a board with two displays has two windows. The windows are shown from the
 * first wait of the program on, vTaskDelay() or dgx_delay(), and in its waits
 * and while pixels go after that; those made before a wait are put side by
 * side, each glass the same whole number of times as large, and may be
 * stretched. Each counts what its own bus sends: two displays on one SPI
 * host of the board share its wire, and their times add up there. A closed
 * window ends the program. bus->dispose() of the screen's bus closes the
 * window.
 *
 * @param width,height The glass, upright; dgx_panel_orientation() turns the screen on it as on a board.
 * @param color_bits   16 or 18.
 * @param options      NULL for the CYD: SPI at 40 MHz.
 * @return The screen, or NULL when there is no window.
 */
dgx_screen_t *dgx_host_screen_init(int width, int height, uint8_t color_bits, const dgx_host_screen_options_t *options);

#ifdef __cplusplus
}
#endif
