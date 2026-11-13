/*
 * The 2.9" e-paper module of black, white and red by WeAct Studio, an
 * SSD1680, on SPI of an ESP32. Drawing changes the picture in memory; the
 * panel shows it after dgx_ssd1680_refresh(), which takes about 20 s, the
 * glass flashing meanwhile.
 *
 * The demo shows a picture in landscape, then the eight ways the screen can
 * lie on the glass, each with its axes, the letter F and the build number
 * (picture.h), and the landscape picture again, which stays: the glass keeps
 * it with no power. Each picture is left for 10 s after its refresh. The log
 * has the time of each refresh.
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus/dgx_spi_esp32.h"
#include "drivers/vscreen.h"

#include "picture.h"

static const char *TAG = "dgx_ssd1680_demo";

/* the wiring, SPI on the pins of VSPI */
enum
{
    PIN_SCK  = 18,
    PIN_MOSI = 23,
    PIN_CS   = 5,
    PIN_DC   = 17,
    PIN_RST  = 16,
    PIN_BUSY = 4,
};

/* the SSD1680 takes 20 MHz at most; a picture is 9.5 KB, 8 ms at 10 MHz */
#define SPI_CLOCK_HZ (10 * 1000 * 1000)

/* the picture to the glass, and the wait until it is there */
static void show(dgx_screen_t *scr, const char *what)
{
    const int64_t t0 = esp_timer_get_time();
    dgx_ssd1680_refresh(scr);
    const int64_t t1 = esp_timer_get_time();
    while (dgx_ssd1680_busy(scr)) vTaskDelay(pdMS_TO_TICKS(10));
    const int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "build %d, %s: sent in %lld ms, on the glass after %lld ms", BUILD, what, (t1 - t0) / 1000, (t2 - t1) / 1000);
    vTaskDelay(pdMS_TO_TICKS(10 * 1000));
}

static void landscape(dgx_screen_t *scr)
{
    dgx_ssd1680_orientation(scr, ways[LANDSCAPE].dir_x, ways[LANDSCAPE].dir_y, ways[LANDSCAPE].swap_xy);
    picture_landscape(scr);
    show(scr, "landscape");
}

void app_main(void)
{
    ESP_LOGI(TAG, "build %d", BUILD);
    dgx_bus_protocols_t *bus = dgx_spi_init(SPI3_HOST, SPI_DMA_CH_AUTO, PIN_MOSI, GPIO_NUM_NC, PIN_SCK, PIN_CS, PIN_DC, SPI_CLOCK_HZ, 0);
    if (!bus) {
        ESP_LOGE(TAG, "SPI bus initialization failed");
        return;
    }
    dgx_screen_t *scr = dgx_ssd1680_init(bus, SSD1680_128X296_BWR, PIN_RST, PIN_BUSY);
    /* the labels of the ways are drawn here first, then put on the glass pixel by pixel */
    dgx_screen_t *aside = dgx_vscreen_init(48, 8, 1, DgxScreenRGB);
    if (!scr || !aside) {
        ESP_LOGE(TAG, "screen initialization failed");
        return;
    }
    landscape(scr);
    for (int way = 0; way < WAYS; ++way) {
        dgx_ssd1680_orientation(scr, ways[way].dir_x, ways[way].dir_y, ways[way].swap_xy);
        picture_way(scr, aside, way);
        show(scr, ways[way].name);
    }
    landscape(scr);
    dgx_screen_destroy(&aside);
    dgx_screen_destroy(&scr);
    ESP_LOGI(TAG, "done: the glass keeps the last picture");
}
