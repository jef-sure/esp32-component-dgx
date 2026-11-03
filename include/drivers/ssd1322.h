#pragma once
#include "dgx_screen.h"
#include "bus/dgx_bus_protocols.h"
#include "dgx_arch.h"
#include "driver/gpio.h"

#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

/**
 * @brief The module: the size of its glass and how it is wired to the controller.
 *
 * The values of all but SSD1322_256X64 come from u8g2 and have not been
 * checked on a module here.
 */
typedef enum {
    SSD1322_256X64,         ///< 256x64: Newhaven NHD-3.12-25664 and the 3.12" clones
    SSD1322_256X64_ZJY,     ///< 256x64 by ZJY, its glass from column 24
    SSD1322_240X128,        ///< 240x128, dual COM
    SSD1322_240X128_TOPWIN, ///< 240x128 by Topwin, single COM
    SSD1322_128X64,         ///< 128x64: Newhaven NHD-2.7-12864WDW3, two segments a pixel
} ssd1322_resolution_t;

/**
 * @brief Set the SSD1322 contrast current, the brightness of what is lit.
 * @param scr      Screen created by dgx_ssd1322_init().
 * @param contrast Contrast current, 0 (dim) to 255 (bright).
 */
void dgx_ssd1322_contrast(dgx_screen_t *scr, uint8_t contrast);

/**
 * @brief Set the SSD1322 master contrast, in sixteenths of the contrast above.
 * @param scr        Screen created by dgx_ssd1322_init().
 * @param brightness Master current, 0 (off) to 15 (full).
 */
void dgx_ssd1322_brightness(dgx_screen_t *scr, uint8_t brightness);

/**
 * @brief Set the gray levels the panel shows, by how long each lights a pixel.
 *
 * The levels grow, each one past the one before it, and none is past 180
 * DCLKs. Level 0 is not set: it has no drive of its own and stays dark; the
 * command takes levels 1 to 15, fifteen bytes. The command of the default
 * linear table is sent instead when @p levels is NULL. The levels of a panel
 * that is on are set at once.
 *
 * @param scr    Screen created by dgx_ssd1322_init().
 * @param levels The 16 levels, gray 0 to 15, or NULL for the default table.
 * @return false when a level is past 180 or not past the one before it; nothing is sent then.
 */
bool dgx_ssd1322_grayscale(dgx_screen_t *scr, const uint8_t *levels);

/**
 * @brief Turn the panel's pixels the other way: all on, all off, or the inverse of the picture.
 *
 * @param scr  Screen created by dgx_ssd1322_init().
 * @param mode 0 normal, 1 all off, 2 all on, 3 inverse.
 */
void dgx_ssd1322_display_mode(dgx_screen_t *scr, uint8_t mode);

/**
 * @brief Turn or mirror the picture.
 *
 * The driver turns the picture in software as it sends it, as on the ST7920:
 * the controller cannot turn a picture that packs two pixels into a byte.
 * When the axes are swapped the screen becomes 64x256 (for 256x64) and is cleared;
 * otherwise what was drawn stays and is sent again turned.
 * (DgxScreenLeftRight, DgxScreenTopBottom, false) is the upright picture the
 * driver starts with.
 *
 * @param scr     Screen created by dgx_ssd1322_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_ssd1322_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Create an SSD1322 16-gray OLED screen on top of a bus (P8 or SPI).
 *
 * The controller writes its memory in groups of four pixels, two to a byte,
 * so the driver keeps the picture in a 4-bit virtual screen (8 KB for
 * 256x64) and sends what has changed row by row, in whole groups. A color
 * is a gray level, 0 (dark) to 15 (bright), in the high nibble: `15 << 4`.
 * The bus needs a buffer of a row of the controller at least, 128 bytes;
 * those of dgx_p8_init() and dgx_spi_init() hold 4000 bytes or more. On the 8-bit parallel bus the
 * controller takes a write cycle of 300 ns at the shortest, 3.3 MHz.
 *
 * @param bus        Bus backend returned by dgx_p8_init() or dgx_spi_init().
 * @param resolution Panel geometry (see ssd1322_resolution_t).
 * @param rst        Reset GPIO (GPIO_NUM_NC if the panel has no reset line).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_ssd1322_init(dgx_bus_protocols_t *bus, ssd1322_resolution_t resolution, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif
