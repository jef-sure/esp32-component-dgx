#pragma once
/*
 * Where a pointer points, as ESP-IDF tells it. The functions are those of a
 * test that sends from memory: it says which of its memory stands for PSRAM.
 */
#include <stdbool.h>

bool esp_ptr_external_ram(const void *p);
bool esp_ptr_dma_capable(const void *p);
bool esp_ptr_dma_ext_capable(const void *p);
