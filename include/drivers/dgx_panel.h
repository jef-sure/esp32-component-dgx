#pragma once
/*
 * dgx_panel.h
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  A panel of the MIPI display command set behind a bus with a window (SPI,
 *  QSPI, 8-bit parallel), made from a description of it: its glass, the
 *  memory of its controller around the glass, the depths and their pixel
 *  formats, the access control of the upright picture, the reset and the
 *  commands that start it. The drivers of DGX for such panels are
 *  descriptions of this kind with their functions of old; a program makes a
 *  screen of a module of its own from a copy of one with what its module has
 *  otherwise: another size of glass and where it lies, another table of
 *  commands from the maker of the module.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bus/dgx_bus_protocols.h"
#include "dgx_screen.h"
#include "dgx_screen_with_bus.h"
#include "drivers/dgx_lcd_init.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Both bounds of the window are sent every time, also one that has not changed: the GC9A01 does not work with one left out. */
#define DGX_PANEL_BOTH_BOUNDS 0x01u
/**
 * The panel shows noise until its memory is written: the backlight is off from
 * the start, the screen is filled with black after the commands of `init`,
 * then the commands of `on` go and the backlight comes on.
 */
#define DGX_PANEL_CLEAR_FIRST 0x02u

/**
 * @brief What a panel is, for dgx_panel_init(). It is kept by its pointer: a description that lives as long as the screen.
 *
 * The glass is width by height pixels upright and lies at x_offset, y_offset
 * of the memory of the controller, which is memory_width by memory_height
 * pixels, or the size of the glass where these are 0. Turned or mirrored,
 * the window lies where the glass is in the memory then: a glass of 240 rows
 * at the top of 320 is at the bottom of them when the rows go up.
 */
typedef struct dgx_panel_t {
    const char *name;                     /**< The name of the screen. */
    uint16_t    width, height;            /**< The glass upright, in pixels. */
    uint16_t    memory_width;             /**< Columns of the memory of the controller, 0 for as many as the glass has. */
    uint16_t    memory_height;            /**< Rows of it, 0 for as many as the glass has. */
    uint16_t    x_offset, y_offset;       /**< Where the glass begins in that memory, upright. */
    uint8_t     colmod[3];                /**< COLMOD for 12, 16 and 18 bits a pixel; 0 for a depth that is not set up. */
    uint8_t     madctl;                   /**< MADCTL of the upright picture with the colors in RGB order; MX, MY, MV and BGR are turned on it. */
    uint8_t     ramrd;                    /**< The command that reads memory, 0 for none. */
    uint8_t     reset_high_ms;            /**< The reset line high so long first; 0 to begin with it low. */
    uint8_t     reset_low_ms;             /**< Then low so long. */
    uint8_t     reset_after_ms;           /**< And high again, waited for so long. */
    uint8_t     flags;                    /**< DGX_PANEL_BOTH_BOUNDS, DGX_PANEL_CLEAR_FIRST. */
    uint16_t    scroll_lines;             /**< Rows of memory the vertical scroll of the panel goes along; 0 for a panel not scrolled so. */
    uint8_t     scroll_area_cmd;          /**< The command that sets the scrolled area, VSCRDEF. */
    uint8_t     scroll_start_cmd;         /**< The command that sets its first row, VSCRSADD. */
    const dgx_lcd_init_cmd_t *init;       /**< The commands after the reset; MADCTL and COLMOD in it take dgx_panel_adj_madctl() and dgx_panel_adj_colmod(). */
    const dgx_lcd_init_cmd_t *on;         /**< With DGX_PANEL_CLEAR_FIRST, the commands once the screen is black; NULL for none. */
} dgx_panel_t;

/**
 * @brief A screen of a panel on a bus.
 * @param bus        The bus of the panel.
 * @param panel      Its description, kept by the pointer.
 * @param rst        The reset line, GPIO_NUM_NC for none.
 * @param backlight  The backlight line, GPIO_NUM_NC for none.
 * @param color_bits 12, 16 or 18, as `panel` has a COLMOD for it.
 * @param cbo        The order of the colors.
 * @return The screen, or NULL for a depth the panel does not take or without memory.
 */
dgx_screen_t *dgx_panel_init(dgx_bus_protocols_t *bus, const dgx_panel_t *panel, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits,
                             dgx_color_order_t cbo);

/** @brief How the screen lies on the panel, by MADCTL; the window follows the glass in the memory of the controller. */
void dgx_panel_orientation(dgx_screen_t *scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy);

/** @brief The backlight on or off; ESP_ERR_NOT_SUPPORTED for a screen made without its line. */
esp_err_t dgx_panel_backlight(dgx_screen_t *scr, bool on);

/** @brief For MADCTL in the commands of `init`: the upright picture of the panel in the order of colors of the screen. */
uint8_t *dgx_panel_adj_madctl(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd);

/** @brief For COLMOD in the commands of `init`: the pixel format of the depth of the screen. */
uint8_t *dgx_panel_adj_colmod(dgx_screen_with_bus_t *scr, const struct _dgx_lcd_init_cmd_t *init_cmd);

#ifdef __cplusplus
}
#endif
