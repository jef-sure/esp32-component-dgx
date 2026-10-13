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
 */
bool dgx_screen_with_bus_window_is_deferred(const dgx_screen_with_bus_t *sbus);

/**
 * @brief Send pixels into the window of such a screen from where the pixels before them ended.
 *
 * @param data    Pixels as DGX keeps them in a buffer.
 * @param lenbits Their length in bits.
 */
void dgx_screen_with_bus_write_window(dgx_screen_with_bus_t *sbus, const uint8_t *data, uint32_t lenbits);

#ifdef __cplusplus
}
#endif
