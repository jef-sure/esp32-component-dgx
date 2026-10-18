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
    SSD1309_128X64,
    SSD1309_128X32
} ssd1309_resolution_t;

/**
 * @brief Set the SSD1309 contrast/brightness.
 * @param scr      Screen created by dgx_ssd1309_init().
 * @param contrast Contrast value, 0 (dim) to 255 (bright).
 */
void dgx_ssd1309_contrast(dgx_screen_t *scr, uint8_t contrast);

/**
 * @brief Tell the driver which segment of the controller the first column of the panel is wired to.
 *
 * Most modules start at segment 0. Some 128x64 modules leave the first two
 * segments unconnected (the "noname2" variant of u8g2): pass 2 for them when
 * the picture comes out two pixels to the right, with a blank stripe on the
 * left. The controller has 128 segments, so the columns the offset pushes
 * past the last one are not shown. The whole picture is sent again right
 * away.
 *
 * @param scr    Screen created by dgx_ssd1309_init().
 * @param offset Segment of the first column, 0 to 127.
 */
void dgx_ssd1309_column_offset(dgx_screen_t *scr, uint8_t offset);

/**
 * @brief Create an SSD1309 monochrome OLED screen on top of a bus (SPI or I2C).
 *
 * The controller is the SSD1306 with a larger charge-pump-free glass driver
 * behind it: the 2.42" 128x64 modules are the usual ones. The driver keeps a
 * 1-bit RAM page buffer; drawing updates RAM and is flushed to the controller
 * via the screen's update_screen function. The init commands are those of the
 * u8g2 driver of the controller.
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_i2c_init().
 * @param resolution Panel geometry (see ssd1309_resolution_t).
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_ssd1309_init(dgx_bus_protocols_t *bus, ssd1309_resolution_t resolution, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif
