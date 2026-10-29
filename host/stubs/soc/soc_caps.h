#pragma once
/* a test that stands for a chip whose 8-bit parallel bus is the LCD_CAM peripheral says so before it includes anything */
#ifdef TEST_LCDCAM
#define SOC_LCDCAM_I80_LCD_SUPPORTED 1
#endif

/* SPI1, the host of the flash, SPI2 and SPI3, as on an ESP32-S3 */
#define SOC_SPI_PERIPH_NUM 3
