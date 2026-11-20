#include <stdlib.h>

#include "bus/dgx_rgb_esp32.h"
#include "dgx_arch.h"
#include "dgx_backlight_priv.h"
#include "drivers/rgb_panel.h"
#include "dgx_screen_with_bus.h"

/* the window of the frame buffer; the glass itself takes no commands */
#define DGX_RGB_CASET 0x2A
#define DGX_RGB_RASET 0x2B
#define DGX_RGB_RAMWR 0x2C

typedef struct _dgx_rgb_panel_screen_t {
    dgx_screen_with_bus_t  base;
    const dgx_rgb_panel_t *panel;
    gpio_num_t             rst;
    gpio_num_t             backlight;
} dgx_rgb_panel_screen_t;

static const char TAG[] = "DGX RGB PANEL";

/*
 * JC8048B070N, the 7-inch glass of the Sunton ESP32-8048S070: 800x480, an
 * EK9716 in DE mode (its MODE pin is high on the board). The timing of the
 * maker's demo for it, 1056 pixel clocks a row and 525 rows a frame; its
 * sheet asks for 29 to 38 MHz. The demo runs it at 16; the board holds 33
 * with the bounce buffers of the bus, 60 frames a second.
 */
const dgx_rgb_panel_t dgx_rgb_panel_jc8048b070n = {
    .name = "JC8048B070N",
    .video =
        {
            .width             = 800,
            .height            = 480,
            .hsync_pulse_width = 30,
            .hsync_back_porch  = 16,
            .hsync_front_porch = 210,
            .vsync_pulse_width = 13,
            .vsync_back_porch  = 10,
            .vsync_front_porch = 22,
            .hsync_idle_low    = true,
            .vsync_idle_low    = true,
            .de_idle_high      = false,
            .pclk_active_neg   = true,
        },
    .pclk_min_hz = 29000000u,
    .pclk_max_hz = 38000000u,
};

static void dgx_rgb_panel_orientation(dgx_screen_t *_scr, dgx_orientation_t dir_x, dgx_orientation_t dir_y, bool swap_xy)
{
    dgx_rgb_panel_screen_t *scr   = (dgx_rgb_panel_screen_t *)_scr;
    const uint16_t          width = scr->panel->video.width, height = scr->panel->video.height;
    _scr->dir_x   = dir_x;
    _scr->dir_y   = dir_y;
    _scr->swap_xy = swap_xy;
    _scr->width   = swap_xy ? height : width;
    _scr->height  = swap_xy ? width : height;
    dgx_screen_unclip(_scr);
    dgx_rgb_orientation(scr->base.bus, dir_x == DgxScreenRightLeft, dir_y == DgxScreenBottomTop, swap_xy);
    /* the bus has set the window to the whole screen */
    scr->base.cached_area.left = scr->base.cached_area.right = 0xffff;
    scr->base.cached_area.top = scr->base.cached_area.bottom = 0xffff;
}

static esp_err_t dgx_rgb_panel_backlight(dgx_screen_t *scr, bool on)
{
    return dgx_backlight_set(((dgx_rgb_panel_screen_t *)scr)->backlight, on);
}

/* the picture is turned by the bus, in its frame buffer */
static const dgx_screen_control_t dgx_rgb_panel_control = {
    .orientation = dgx_rgb_panel_orientation,
    .backlight   = dgx_rgb_panel_backlight,
};

dgx_screen_t *dgx_rgb_panel_init(dgx_bus_protocols_t *bus, const dgx_rgb_panel_t *panel, gpio_num_t rst, gpio_num_t backlight, uint8_t color_bits,
                                 dgx_color_order_t cbo, dgx_rgb_frames_t frames)
{
    if (!bus || bus->bus_type != DGX_BUS_RGB) {
        ESP_LOGE(TAG, "the display needs a bus made by dgx_rgb_init()");
        return NULL;
    }
    if (!panel || !panel->video.width || !panel->video.height) {
        ESP_LOGE(TAG, "no glass given");
        return NULL;
    }
    if (color_bits != 16) {
        ESP_LOGE(TAG, "%u bits a pixel: the bus has the 16 lines of RGB565", color_bits);
        return NULL;
    }
    if (frames != DgxRgbOneFrame && frames != DgxRgbTwoFrames) {
        ESP_LOGE(TAG, "%d frame buffers: one or two", (int)frames);
        return NULL;
    }
    dgx_rgb_panel_screen_t *scr = (dgx_rgb_panel_screen_t *)calloc(1, sizeof(dgx_rgb_panel_screen_t));
    if (!scr) {
        ESP_LOGE(TAG, "Screen structure memory allocation failed");
        return NULL;
    }
    scr->panel                    = panel;
    scr->rst                      = rst;
    scr->backlight                = backlight;
    scr->base.bus                 = bus;
    scr->base.scr.cg_row_shift    = 0;
    scr->base.scr.cg_col_shift    = 0;
    scr->base.scr.width           = panel->video.width;
    scr->base.scr.height          = panel->video.height;
    scr->base.scr.color_bits      = color_bits;
    scr->base.scr.rgb_order       = cbo;
    scr->base.scr.screen_name     = panel->name;
    scr->base.scr.screen_submodel = 0;
    scr->base.scr.screen_subtype  = DgxPhysicalScreenWithBus;
    scr->base.scr.control         = &dgx_rgb_panel_control;
    dgx_screen_with_bus_init_area(&scr->base, DGX_RGB_CASET, DGX_RGB_RASET, DGX_RGB_RAMWR, DGX_SCREEN_AREA_PROTO_STD16);
    if (rst >= 0) {
        dgx_gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        dgx_gpio_set_level(rst, 0);
        dgx_delay(10);
        dgx_gpio_set_level(rst, 1);
        dgx_delay(20);
    }
    const uint32_t pclk = dgx_rgb_pixel_clock(bus);
    if ((panel->pclk_min_hz && pclk < panel->pclk_min_hz) || (panel->pclk_max_hz && pclk > panel->pclk_max_hz)) {
        ESP_LOGI(TAG, "%s: a pixel clock of %u kHz, its sheet allows %u to %u kHz", panel->name, (unsigned)(pclk / 1000u),
                 (unsigned)(panel->pclk_min_hz / 1000u), (unsigned)(panel->pclk_max_hz / 1000u));
    }
    /* the frame buffer is black and sent from now on */
    if (!dgx_rgb_video_setup(bus, &panel->video, cbo == DgxScreenBGR, (uint8_t)frames)) {
        free(scr);
        return NULL;
    }
    /* where dgx_rgb_bench() starts from, and what it measures against */
    dgx_rgb_clock_range(bus, panel->pclk_min_hz, panel->pclk_max_hz);
    dgx_rgb_screen_functions((dgx_screen_t *)scr);
    dgx_backlight_on_start(backlight);
    return (dgx_screen_t *)scr;
}
