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
 * @brief The module: the size of its glass, which shows white, black and red.
 *
 * The sizes are those of the glass as the controller scans it, sources by
 * gates: the 2.9" module is 128 pixels across and 296 down, and lies in
 * landscape as 296x128 once turned by dgx_ssd1680_orientation().
 */
typedef enum {
    SSD1680_128X296_BWR, ///< 2.9": WeAct Studio 2.9" Epaper Module with red, Good Display GDEM029C90
    SSD1680_122X250_BWR, ///< 2.13": WeAct Studio 2.13" Epaper Module with red, Good Display GDEY0213Z98; not checked on a module
} ssd1680_resolution_t;

/**
 * @brief Send the picture to the panel and start showing it.
 *
 * The whole picture is sent, about 9.5 KB for the 2.9" module, and the
 * panel changes all of it: the glass flashes several times, which takes
 * about 20 s at 23 °C on the 2.9" module, 15 s on the 2.13", longer in the
 * cold. The call returns once the picture is sent. Drawing may go on at
 * once; the next call of the driver waits until the panel has finished,
 * and the controller then goes into deep sleep until the next refresh,
 * when the reset line is wired.
 *
 * The glass keeps the picture with no power at all. The maker of the glass
 * tests it with a refresh every 150 s; more often than every few minutes
 * wears it.
 *
 * @param scr Screen created by dgx_ssd1680_init().
 */
void dgx_ssd1680_refresh(dgx_screen_t *scr);

/**
 * @brief Tell whether the panel is still showing the picture of the last refresh.
 *
 * Once it has finished, the controller is put into deep sleep, when the
 * reset line is wired, and the call returns false. A program that has to
 * wait for the end waits while this is true.
 *
 * @param scr Screen created by dgx_ssd1680_init().
 * @return true while the BUSY line is high after dgx_ssd1680_refresh().
 */
bool dgx_ssd1680_busy(dgx_screen_t *scr);

/**
 * @brief Set how the screen lies on the panel (rotation/mirroring).
 *
 * The picture is kept in the layout of the memory of the controller, and a
 * pixel goes to its place there as it is drawn. What is drawn before the
 * call stays where it is on the glass; what is drawn after goes the new
 * way. With @p swap_xy the screen has its sides exchanged: 296x128 for the
 * 128x296 of the 2.9" module. (DgxScreenLeftRight, DgxScreenTopBottom,
 * false) is the screen the driver starts with, the memory as the
 * controller scans it. The 2.9" module of WeAct Studio lying with its pins
 * on the right is (DgxScreenLeftRight, DgxScreenBottomTop, true), 296x128.
 *
 * @param scr     Screen created by dgx_ssd1680_init().
 * @param dir_x   DgxScreenLeftRight or DgxScreenRightLeft.
 * @param dir_y   DgxScreenTopBottom or DgxScreenBottomTop.
 * @param swap_xy Swap the X and Y axes.
 */
void dgx_ssd1680_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/**
 * @brief Create a screen of an SSD1680 e-paper panel of three colors on an SPI bus.
 *
 * Drawing changes the picture in memory only, two planes of a bit a pixel
 * as the controller takes them, 9.5 KB for the 2.9" module; the panel
 * shows it after dgx_ssd1680_refresh(). A new screen is white.
 *
 * A color is one of the three, a byte a pixel: DGX_BWR_BLACK, DGX_BWR_WHITE,
 * DGX_BWR_RED (dgx_bits.h); a value above them is red too.
 * dgx_rgb_to_bwr() gives the nearest of them to a color by its red, green
 * and blue, so DGX_RED(dgx_rgb_to_bwr) and the other colors of dgx_colors.h
 * work. The colors are not levels that could be mixed: draw a font of soft
 * edges with DGX_FONT_MONO, as on any screen whose pixels are indices.
 *
 * The controller takes a clock of 20 MHz at most. Init resets the
 * controller and waits on BUSY for it to come up: a BUSY line that stays
 * high keeps it waiting, with an error in the log every minute.
 *
 * @param bus        Bus backend returned by dgx_spi_init().
 * @param resolution The module (see ssd1680_resolution_t).
 * @param rst        Reset GPIO; GPIO_NUM_NC if the line is not wired: the controller then never sleeps, for only a
 *                   reset wakes it.
 * @param busy       BUSY GPIO, an input; it has to be wired.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy(), which waits for a refresh
 *         under way to end.
 */
dgx_screen_t *dgx_ssd1680_init(dgx_bus_protocols_t *bus, ssd1680_resolution_t resolution, gpio_num_t rst, gpio_num_t busy);

#ifdef __cplusplus
// @formatter:off
    }
    // @formatter:on
#endif
