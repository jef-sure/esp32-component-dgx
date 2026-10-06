/* Host stand-in for dgx_spi_area_protocol.c, which needs the ESP32 SPI driver. */
#include "dgx_spi_area_protocol.h"

void dgx_screen_with_bus_set_area_window(dgx_screen_with_bus_t *sbus, uint16_t left, uint16_t right, uint16_t top, uint16_t bottom)
{
    (void)sbus, (void)left, (void)right, (void)top, (void)bottom;
}
