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
    SH1106_128X64,
    SH1106_64X32
} sh1106_resolution_t;

/**
 * @brief Set the SH1106 contrast/brightness.
 * @param scr      Screen created by dgx_sh1106_init().
 * @param contrast Contrast value, 0 (dim) to 255 (bright).
 */
void dgx_sh1106_contrast(dgx_screen_t *scr, uint8_t contrast);

/**
 * @brief Tell the driver which segment of the controller the first column of the panel is wired to.
 *
 * The controller has 132 segments and the glass takes a run of them: the
 * 128x64 modules start at segment 2, the 64x32 ones at 32, which the
 * constructor sets. Pass another value when the picture comes out shifted,
 * with a blank stripe on one side. The columns the offset pushes past the
 * last segment are not shown. The whole picture is sent again right away.
 *
 * @param scr    Screen created by dgx_sh1106_init().
 * @param offset Segment of the first column, 0 to 131.
 */
void dgx_sh1106_column_offset(dgx_screen_t *scr, uint8_t offset);

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
 * @param scr     Screen created by dgx_sh1106_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_sh1106_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Create an SH1106 monochrome OLED screen on top of a bus (SPI or I2C).
 *
 * The controller of most 1.3" 128x64 modules, often sold as SSD1306: the
 * same commands, but 132 segments and the page addressing mode only, so the
 * driver sends a changed rectangle page by page. It keeps a 1-bit RAM page
 * buffer; drawing updates RAM and is flushed to the controller via the
 * screen's update_screen function. The init commands are those of the u8g2
 * driver of the controller.
 *
 * @param bus        Bus backend returned by dgx_spi_init() or dgx_i2c_init().
 * @param resolution Panel geometry (see sh1106_resolution_t).
 * @param rst        Reset GPIO (use GPIO_NUM_NC if the panel has no reset line).
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_sh1106_init(dgx_bus_protocols_t *bus, sh1106_resolution_t resolution, gpio_num_t rst);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif
