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

/*
 * The values begin at 256, a number no chip has a pin of: a call in the order of
 * before DGX 0.12, with the resolution second, is refused, not taken for a pin.
 */
typedef enum dgx_ssd1309_resolution_t {
    DGX_SSD1309_128X64 = 256,
    DGX_SSD1309_128X32,
    /* deprecated: the names of before, the same values */
    SSD1309_128X64 DGX_DEPRECATED("use DGX_SSD1309_128X64") = DGX_SSD1309_128X64,
    SSD1309_128X32 DGX_DEPRECATED("use DGX_SSD1309_128X32") = DGX_SSD1309_128X32,
} dgx_ssd1309_resolution_t;

/** The name the type had before; deprecated until DGX 1.0. */
typedef dgx_ssd1309_resolution_t ssd1309_resolution_t DGX_DEPRECATED("use dgx_ssd1309_resolution_t");

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
 * @param scr     Screen created by dgx_ssd1309_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 * @deprecated Use dgx_screen_orientation().
 */
DGX_DEPRECATED("use dgx_screen_orientation()")
void dgx_ssd1309_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

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
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @param resolution Panel geometry (see dgx_ssd1309_resolution_t).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_ssd1309_init(dgx_bus_protocols_t *bus, gpio_num_t rst, dgx_ssd1309_resolution_t resolution);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif
