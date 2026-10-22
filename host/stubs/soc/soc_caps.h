#pragma once
/* a test that stands for a chip whose 8-bit parallel bus is the LCD_CAM peripheral says so before it includes anything */
#ifdef TEST_LCDCAM
#define SOC_LCDCAM_I80_LCD_SUPPORTED 1
#endif
