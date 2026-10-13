#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen_with_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

void dgx_screen_with_bus_set_area_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom);
void dgx_screen_with_bus_set_area_window_full(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top,
											  uint16_t bottom);

/**
 * @brief Tell whether the window of a screen is sent with its pixels and not when it is set.
 *
 * A bus of an ESP32 or an ESP32-S2 for an 8-bit parallel display sends a
 * command before every block of data, there is no transfer of data alone. So
 * on DGX_BUS_P8 every block of pixels goes in one transfer with the command
 * that writes memory, and a block that goes on where the one before ended has
 * its window set anew, to what is left of it.
 *
 * So does, on any bus, a window that the scroll of the panel cuts: its rows
 * do not lie in memory one after another, and it is sent in pieces that do.
 */
bool dgx_screen_with_bus_window_is_deferred(const dgx_screen_with_bus_t *sbus);

/**
 * @brief Send pixels into the window of such a screen from where the pixels before them ended.
 *
 * @param data    Pixels as DGX keeps them in a buffer.
 * @param lenbits Their length in bits.
 */
void dgx_screen_with_bus_write_window(dgx_screen_with_bus_t *sbus, const uint8_t *data, uint32_t lenbits);

/**
 * @brief Tell that the controller of a screen scrolls an area of its rows itself; called by its driver once.
 *
 * It is the scroll of the MIPI DCS: one command sets the rows above the
 * area, the rows of the area and the rows below it, three numbers of two
 * bytes that make up all the rows of memory; another sets the row of memory
 * the area is shown from.
 *
 * @param lines     Rows of the memory of the controller along which it scrolls.
 * @param area_cmd  The command that sets the area.
 * @param start_cmd The command that sets the row of memory the area is shown from.
 */
void dgx_screen_with_bus_scroll_init(dgx_screen_with_bus_t *sbus, uint16_t lines, uint8_t area_cmd, uint8_t start_cmd);

/**
 * @brief Tell how the rows the controller scrolls lie on the screen; called by its driver when the orientation is set.
 *
 * A scroll in force is given up: the picture is where memory has it again.
 *
 * @param columns The rows of memory are the columns of the screen: the axes are swapped.
 * @param mirror  Addresses along them run against the rows of memory.
 */
void dgx_screen_with_bus_scroll_way(dgx_screen_with_bus_t *sbus, bool columns, bool mirror);

/**
 * @brief What dgx_scroll() does on a display that is not read: by the scroll of the panel, when the panel can.
 *
 * The panel can when its driver has told of its scroll, the move goes along
 * the rows of its memory, the rectangle takes the screen from edge to edge
 * across them, and no other area is scrolled at the moment: a panel has one.
 * The strip that leaves is filled first, so what comes in at the other end
 * is the color already. An area is let go when a move as large as it fills
 * it.
 *
 * The rectangle is one cut by the screen, and the move is not nothing.
 *
 * @return false when the picture had to be moved and the panel cannot move it.
 */
bool dgx_screen_with_bus_scroll(dgx_screen_with_bus_t *sbus, int x, int y, int w, int h, int dx, int dy, uint32_t color);

#ifdef __cplusplus
}
#endif
