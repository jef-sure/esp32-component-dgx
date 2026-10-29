/* test_spi.c for an ESP32-P4, whose GDMA does not align by itself: DMA from PSRAM asks 16 bytes under flash encryption */
#define SOC_AHB_GDMA_VERSION 2
#include "test_spi.c"
