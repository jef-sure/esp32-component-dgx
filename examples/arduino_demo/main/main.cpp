#include <Arduino.h>

#include "bus/dgx_spi_esp32.h"
#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"
#include "dgx_font.h"
#include "drivers/ili9341.h"
#include "fonts/DejaVuSansBook32.h"
#include "fonts/LiberationSansRegular12.h"

// The display of a CYD, on SPI2_HOST; its panel has no reset line of its
// own. Arduino's own SPI object drives SPI3_HOST on an ESP32, the host the
// SD card slot of the CYD is wired to: the sketch may use it for the card,
// the display keeps to the other host.
static const spi_host_device_t LCD_HOST = SPI2_HOST;
static const gpio_num_t PIN_MOSI      = GPIO_NUM_13;
static const gpio_num_t PIN_MISO      = GPIO_NUM_12;
static const gpio_num_t PIN_CLK       = GPIO_NUM_14;
static const gpio_num_t PIN_CS        = GPIO_NUM_15;
static const gpio_num_t PIN_DC        = GPIO_NUM_2;
static const gpio_num_t PIN_RST       = GPIO_NUM_NC;
static const gpio_num_t PIN_BACKLIGHT = GPIO_NUM_21;

static dgx_screen_t *screen;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return dgx_rgb_to_16(r, g, b);
}

void setup()
{
    Serial.begin(115200);
    dgx_bus_protocols_t *bus =
        dgx_spi_init(LCD_HOST, SPI_DMA_CH_AUTO, PIN_MOSI, PIN_MISO, PIN_CLK, PIN_CS, PIN_DC, 40 * 1000 * 1000, 0);
    if (!bus) {
        Serial.println("dgx_spi_init failed");
        return;
    }
    screen = dgx_ili9341_init(bus, PIN_RST, PIN_BACKLIGHT, 16, DgxScreenRGB);
    if (!screen) {
        Serial.println("dgx_ili9341_init failed");
        bus->dispose(bus);
        return;
    }
    dgx_ili9341_orientation(screen, DgxScreenRightLeft, DgxScreenTopBottom, true); // landscape, 320x240
    Serial.printf("screen %d x %d\n", screen->width, screen->height);

    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, rgb(6, 10, 20));
    dgx_font_string_utf8_screen(screen, 10, 24, "DGX in an Arduino sketch", DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1,
                                LiberationSansRegular12(), NULL, NULL);
    dgx_draw_circle(screen, 240, 140, 60, rgb(48, 140, 255));
}

void loop()
{
    if (!screen)
        return;
    static float angle;
    const int cx = 240, cy = 140, r = 54;

    // the seconds hand: rub out the old one, draw the new one
    dgx_draw_line_thick(screen, cx, cy, cx + r * cosf(angle), cy + r * sinf(angle), 3, rgb(6, 10, 20));
    angle = (millis() / 1000 % 60) * (float)(2 * PI / 60) - (float)(PI / 2);
    dgx_draw_line_thick(screen, cx, cy, cx + r * cosf(angle), cy + r * sinf(angle), 3, rgb(255, 200, 40));

    char text[16];
    snprintf(text, sizeof text, "%lu s", (unsigned long)(millis() / 1000));
    dgx_fill_rectangle(screen, 10, 110, 150, 44, rgb(6, 10, 20));
    dgx_font_string_utf8_screen(screen, 10, 150, text, DGX_WHITE(dgx_rgb_to_16), DgxOutputNormal, 1, DejaVuSansBook32(), NULL, NULL);
    delay(200);
}
