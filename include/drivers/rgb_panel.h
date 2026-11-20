#pragma once
#include "dgx_screen.h"
#ifdef __cplusplus
// @formatter:off
extern "C" {
// @formatter:on
#endif

#include "bus/dgx_bus_protocols.h"
#include "bus/dgx_rgb_esp32.h"
#include "driver/gpio.h"

/**
 * @brief The glass of a parallel RGB display: its size, its timing and the pixel clock its sheet allows.
 *
 * An RGB panel takes no commands: its controller (EK9716, ST7262, ILI6122
 * and the like) is set up by its pins on the glass, and a program sees
 * only the size and the timing. So a panel is described, not driven, and
 * another glass is another description.
 */
typedef struct _dgx_rgb_panel_t {
    const char     *name;        /*!< the glass, as its sheet names it */
    dgx_rgb_video_t video;       /*!< size, timing and the levels of its lines */
    uint32_t        pclk_min_hz; /*!< the pixel clock its sheet allows; 0 when it is not known */
    uint32_t        pclk_max_hz;
} dgx_rgb_panel_t;

/**
 * @brief The 7-inch 800x480 TN glass of the Sunton ESP32-8048S070, JC8048B070N with an EK9716, in DE mode.
 *
 * The timing is that of the maker's demo; the sheet allows a pixel clock of
 * 29 to 38 MHz. The board holds 33, 60 frames a second.
 */
extern const dgx_rgb_panel_t dgx_rgb_panel_jc8048b070n;

/** @brief How many frame buffers the display has. */
typedef enum {
    DgxRgbOneFrame  = 1, /*!< drawing shows at the next pass of the scan, and a moving picture may tear */
    DgxRgbTwoFrames = 2, /*!< a batch of drawing is shown whole at the start of a frame, for one more frame buffer in PSRAM */
} dgx_rgb_frames_t;

/**
 * @brief Create a screen of a glass on a parallel RGB bus.
 *
 * The frame buffers are in PSRAM, 750 KB each for 800x480; the screen is
 * black and the backlight on when it returns. When the pixel clock of the bus lies
 * outside what the sheet of the glass allows, the log says so once: many
 * boards run their glass below it, as fast as their PSRAM brings the pixels.
 *
 * @param bus        Bus from dgx_rgb_init().
 * @param panel      The glass, such as &dgx_rgb_panel_jc8048b070n.
 * @param rst        Reset GPIO, low for reset (GPIO_NUM_NC when the glass has none, as on most RGB modules).
 * @param backlight  Backlight GPIO, high for on (GPIO_NUM_NC when it is not under control). dgx_screen_backlight() turns it off and on.
 * @param color_bits 16: the bus has 16 data lines of RGB565.
 * @param cbo        Color channel order (DgxScreenRGB or DgxScreenBGR); BGR exchanges red and blue where pixels are put.
 * @param frames     DgxRgbOneFrame, or DgxRgbTwoFrames for a picture shown whole.
 * @return Newly allocated screen, or NULL on failure. Free with dgx_screen_destroy().
 */
dgx_screen_t *dgx_rgb_panel_init(dgx_bus_protocols_t *bus, const dgx_rgb_panel_t *panel, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits,
                                 dgx_color_order_t cbo, dgx_rgb_frames_t frames);

#ifdef __cplusplus
// @formatter:off
}
// @formatter:on
#endif
