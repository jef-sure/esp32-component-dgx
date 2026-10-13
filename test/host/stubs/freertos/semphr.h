#pragma once
/*
 * A binary semaphore for the host: there is one thread, so taking one that
 * was not given would wait for ever. That is a mistake of the code under
 * test, and it ends the test.
 */
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"

typedef int *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (SemaphoreHandle_t)calloc(1, sizeof(int)); }
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken)
{
    *s = 1;
    if (woken) *woken = pdFALSE;
    return pdTRUE;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks)
{
    (void)ticks;
    if (!*s) {
        fprintf(stderr, "xSemaphoreTake: the semaphore was not given, the task would wait for ever\n");
        exit(EXIT_FAILURE);
    }
    *s = 0;
    return pdTRUE;
}
