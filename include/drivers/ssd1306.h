#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "driver/gpio.h"
#include "dgx_arch.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

typedef enum {
    SSD1306_128X32,
    SSD1306_128X64,
    SSD1306_96X16,
    SSD1306_64X48,
    SSD1306_72X40
} ssd1306_resolution_t;

/**
 * @brief Set the SSD1306 contrast/brightness.
 * @param scr      Screen created by dgx_ssd1306_init().
 * @param contrast Contrast value, 0 (dim) to 255 (bright).
 */
void dgx_ssd1306_contrast(dgx_screen_t *scr, uint8_t contrast);

/**
 * @brief Set how the screen lies on the panel (rotation/mirroring).
 *
 * The driver turns the picture in software as it sends it, as on the
 * ST7920: the controller mirrors by its segments and rows only as a module
 * wires its glass. With @p swap_xy the screen has its sides exchanged, 64x128
 * for 128x64, and what was drawn before the call is cleared, for its rows are of
 * another length; without a change of size the picture stays and is sent
 * again as it lies now. A turned screen is gathered bit by bit as it is
 * sent. (DgxScreenLeftRight, DgxScreenTopBottom, false) is the picture the
 * driver starts with.
 *
 * @param scr     Screen created by dgx_ssd1306_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_ssd1306_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Create an SSD1306 monochrome OLED screen on top of a bus (SPI or I2C).
 *
 * The driver keeps a 1-bit RAM page buffer; drawing updates RAM and is flushed
 * to the controller via the screen's update_screen function.
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_i2c_init().
 * @param resolution Panel geometry (see ssd1306_resolution_t).
 * @param is_ext_vcc Non-zero if the panel is driven from an external VCC supply.
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_ssd1306_init(dgx_bus_protocols_t *bus, ssd1306_resolution_t resolution, uint8_t is_ext_vcc, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif

