/*
 * rgb_advice.c
 *
 *  Copyright (c) 2021-2026 Anton Petrusevich
 *      Author: Anton Petrusevich
 *
 *  What dgx_rgb_bench() found, read back: the pixel clock to give
 *  dgx_rgb_init(), and the rest in words. Nothing here touches the chip, so
 *  it is built on the computer as well and tested there.
 */
#include <stdarg.h>
#include <stdio.h>

#include "bus/dgx_rgb_esp32.h"

/* the step that held: the last one the bench tried, as it stops at the first that holds */
static const dgx_rgb_bench_step_t *dgx_rgb_held(const dgx_rgb_bench_t *bench)
{
    if (!bench || !bench->steps || bench->steps > DGX_RGB_BENCH_STEPS) return NULL;
    for (uint8_t i = 0; i < bench->steps; ++i)
        if (bench->step[i].holds) return &bench->step[i];
    return NULL;
}

uint32_t dgx_rgb_advise(const dgx_rgb_bench_t *bench)
{
    const dgx_rgb_bench_step_t *held = dgx_rgb_held(bench);
    return held ? held->pclk_hz : 0;
}

/* append to a text that may be cut: `*at` and `*left` move on, `*length` counts the whole */
static void dgx_rgb_text_add(char **at, size_t *left, int *length, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(*at, *left, format, args);
    va_end(args);
    if (n < 0) return;
    *length += n;
    const size_t taken = (size_t)n < *left ? (size_t)n : (*left ? *left - 1u : 0u);
    *at += taken;
    *left -= taken;
}

/* a clock as MHz with a tenth */
#define DGX_RGB_MHZ(hz) (unsigned)((hz) / 1000000u), (unsigned)((hz) / 100000u % 10u)

int dgx_rgb_bench_text(const dgx_rgb_bench_t *bench, char *text, size_t size)
{
    char  *at     = text;
    size_t left   = size;
    int    length = 0;
    if (size) text[0] = 0;
    if (!bench || !bench->steps || bench->steps > DGX_RGB_BENCH_STEPS) {
        dgx_rgb_text_add(&at, &left, &length, "Not measured.");
        return length;
    }
    const dgx_rgb_bench_step_t *held = dgx_rgb_held(bench), *top = &bench->step[0];
    if (held) {
        dgx_rgb_text_add(&at, &left, &length, "At %u.%u MHz, %u.%u frames a second, the bus keeps up: the copies take %u%% of core %u with the picture still and %u%% while drawing.",
                         DGX_RGB_MHZ(held->pclk_hz), (unsigned)(held->frames_x10 / 10u), (unsigned)(held->frames_x10 % 10u), (unsigned)held->still_pct,
                         (unsigned)bench->display_core, (unsigned)held->drawing_pct);
        if (held != top) {
            dgx_rgb_text_add(&at, &left, &length, " At %u.%u MHz it did not: a copy came %u us after the one before, past two periods of %u.", DGX_RGB_MHZ(top->pclk_hz),
                             (unsigned)top->gap_us, (unsigned)top->period_us);
        }
        if (bench->sheet_min_hz && held->pclk_hz < bench->sheet_min_hz) {
            dgx_rgb_text_add(&at, &left, &length, " That is below the %u.%u MHz the glass asks for.", DGX_RGB_MHZ(bench->sheet_min_hz));
        }
    } else {
        const dgx_rgb_bench_step_t *last = &bench->step[bench->steps - 1];
        dgx_rgb_text_add(&at, &left, &length, "Down to %u.%u MHz the bus did not keep up: a copy came %u us after the one before, past two periods of %u.",
                         DGX_RGB_MHZ(last->pclk_hz), (unsigned)last->gap_us, (unsigned)last->period_us);
    }
    if (bench->flags & DGX_RGB_BENCH_OTHER_CORE) {
        dgx_rgb_text_add(&at, &left, &length, " The display was made on core %u and drawn from core %u: make it on core %u, where the copy stops the drawing while it works.",
                         (unsigned)bench->display_core, (unsigned)bench->drawing_core, (unsigned)bench->drawing_core);
    } else if (held != top) {
        dgx_rgb_text_add(&at, &left, &length, " The display is on the core that draws, so what holds it back is work in PSRAM on the other core.");
    }
    if (bench->flags & DGX_RGB_BENCH_NO_XIP) {
        dgx_rgb_text_add(&at, &left, &length, " CONFIG_SPIRAM_XIP_FROM_PSRAM is off: writing to the flash spoils the picture.");
    }
    if (bench->flags & DGX_RGB_BENCH_RESTART) {
        dgx_rgb_text_add(&at, &left, &length, " CONFIG_LCD_RGB_RESTART_IN_VSYNC is set with an IDF that keeps on counting: after one late copy the picture keeps stripes; leave it off.");
    }
    return length;
}
