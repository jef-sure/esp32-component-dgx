/*
 * dgx_rgb_advise() and dgx_rgb_bench_text(): what dgx_rgb_bench() found, read back. The clock that held is
 * the one to give dgx_rgb_init(), none when none did; the text names it, the clock above it that did not,
 * and what to mend, and is cut as snprintf() cuts.
 */
#include <string.h>

#include "bus/dgx_rgb_esp32.h"
#include "check.h"

static dgx_rgb_bench_t board(void)
{
    dgx_rgb_bench_t b = {.version = DGX_RGB_BENCH_VERSION, .size = sizeof(dgx_rgb_bench_t), .width = 800, .height = 480};
    b.pclk_hz = 33000000, b.sheet_min_hz = 29000000, b.sheet_max_hz = 38000000;
    return b;
}

static void step(dgx_rgb_bench_t *b, uint32_t hz, uint16_t frames_x10, uint16_t period, uint16_t gap, uint8_t still, uint8_t drawing)
{
    dgx_rgb_bench_step_t *s = &b->step[b->steps++];
    s->pclk_hz = hz, s->frames_x10 = frames_x10, s->period_us = period, s->gap_us = gap, s->still_pct = still, s->drawing_pct = drawing;
    s->holds = gap <= 2u * period;
}

int main(void)
{
    char text[720];

    /* not measured: no clock, and the text says so */
    dgx_rgb_bench_t none = board();
    CHECK(dgx_rgb_advise(&none) == 0);
    CHECK(dgx_rgb_advise(NULL) == 0);
    dgx_rgb_bench_text(&none, text, sizeof(text));
    CHECK(strcmp(text, "Not measured.") == 0);

    /* the top of the glass holds at once, the display on the core that draws, XIP on */
    dgx_rgb_bench_t top = board();
    step(&top, 38000000, 685, 277, 280, 14, 33);
    CHECK(dgx_rgb_advise(&top) == 38000000);
    dgx_rgb_bench_text(&top, text, sizeof(text));
    CHECK(strstr(text, "At 38.0 MHz, 68.5 frames a second, the bus keeps up") != NULL);
    CHECK(strstr(text, "14% of core 0") != NULL && strstr(text, "33% while drawing") != NULL);
    CHECK(strstr(text, "did not") == NULL && strstr(text, "core 1") == NULL && strstr(text, "CONFIG_") == NULL);

    /* the display on the other core: the clocks above fail, the one below the glass holds, and the text says where to make it */
    dgx_rgb_bench_t other = board();
    other.display_core = 1, other.drawing_core = 0, other.flags = DGX_RGB_BENCH_OTHER_CORE | DGX_RGB_BENCH_NO_XIP;
    step(&other, 38000000, 685, 277, 980, 14, 90);
    step(&other, 33200000, 600, 318, 990, 13, 89);
    step(&other, 29000000, 525, 364, 880, 11, 70);
    step(&other, 25400000, 459, 415, 870, 9, 60);
    step(&other, 22200000, 401, 475, 650, 8, 14);
    CHECK(dgx_rgb_advise(&other) == 22200000);
    const int whole = dgx_rgb_bench_text(&other, text, sizeof(text));
    CHECK(whole == (int)strlen(text) && whole < (int)sizeof(text));
    CHECK(strstr(text, "At 22.2 MHz") != NULL);
    CHECK(strstr(text, "At 38.0 MHz it did not: a copy came 980 us after the one before, past two periods of 277.") != NULL);
    CHECK(strstr(text, "below the 29.0 MHz the glass asks for") != NULL);
    CHECK(strstr(text, "made on core 1 and drawn from core 0: make it on core 0") != NULL);
    CHECK(strstr(text, "CONFIG_SPIRAM_XIP_FROM_PSRAM is off") != NULL);

    /* on the core that draws, a clock above fails: the other core's work in PSRAM is named */
    dgx_rgb_bench_t load = board();
    step(&load, 38000000, 685, 277, 700, 14, 60);
    step(&load, 33200000, 600, 318, 500, 13, 31);
    CHECK(dgx_rgb_advise(&load) == 33200000);
    dgx_rgb_bench_text(&load, text, sizeof(text));
    CHECK(strstr(text, "work in PSRAM on the other core") != NULL && strstr(text, "made on core") == NULL);

    /* nothing holds; the restart that keeps on counting is named */
    dgx_rgb_bench_t bad = board();
    bad.flags = DGX_RGB_BENCH_RESTART;
    step(&bad, 38000000, 685, 277, 2000, 14, 95);
    step(&bad, 33200000, 600, 318, 2000, 13, 95);
    CHECK(dgx_rgb_advise(&bad) == 0);
    dgx_rgb_bench_text(&bad, text, sizeof(text));
    CHECK(strstr(text, "Down to 33.2 MHz the bus did not keep up") != NULL);
    CHECK(strstr(text, "CONFIG_LCD_RGB_RESTART_IN_VSYNC is set") != NULL);

    /* cut short: still a string, and the length of the whole */
    char small[24];
    dgx_rgb_bench_text(&other, text, sizeof(text));
    memset(small, 'x', sizeof(small));
    const int cut = dgx_rgb_bench_text(&other, small, sizeof(small));
    CHECK(cut == whole && strlen(small) == sizeof(small) - 1 && strncmp(small, text, sizeof(small) - 1) == 0);
    CHECK(dgx_rgb_bench_text(&other, NULL, 0) == whole);

    /* steps past the table are not read */
    dgx_rgb_bench_t broken = top;
    broken.steps = DGX_RGB_BENCH_STEPS + 1;
    CHECK(dgx_rgb_advise(&broken) == 0);
    CHECK_DONE();
}
