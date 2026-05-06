file(GLOB dgx_font_sources
	"${COMPONENT_DIR}/src/fonts/*.c"
)

set(srcs "src/drivers/dgx_lcd_init.c" "src/dgx_bus_optimized_funcs.c" "src/dgx_spi_area_protocol.c" "src/draw.c" "src/font.c" "src/font_morph.c" "src/orientation.c" "src/bitmap.c" "src/gauge.c")

list(APPEND srcs ${dgx_font_sources})

if(CONFIG_DGX_ENABLE_SPI)
	list(APPEND srcs "src/bus/spi_esp32.c")
endif()

if(CONFIG_DGX_ENABLE_I2C)
	list(APPEND srcs "src/bus/i2c_esp32.c")
endif()

if(CONFIG_DGX_ENABLE_P8)
	list(APPEND srcs "src/bus/p8_esp32.c")
endif()

if(CONFIG_DGX_ENABLE_SPI_ST7735)
	list(APPEND srcs "src/drivers/st7735.c")
endif()

if(CONFIG_DGX_ENABLE_SPI_ST7789)
	list(APPEND srcs "src/drivers/st7789.c")
endif()

if(CONFIG_DGX_ENABLE_SPI_GC9A01)
	list(APPEND srcs "src/drivers/gc9a01.c")
endif()

if(CONFIG_DGX_ENABLE_SPI_ILI_9341)
	list(APPEND srcs "src/drivers/ili9341.c")
endif()

if(CONFIG_DGX_ENABLE_ST7920)
	list(APPEND srcs "src/drivers/st7920.c")
endif()

if(CONFIG_DGX_ENABLE_ST7565R)
	list(APPEND srcs "src/drivers/st7565r.c")
endif()

if(CONFIG_DGX_ENABLE_SSD1351)
	list(APPEND srcs "src/drivers/ssd1351.c")
endif()

if(CONFIG_DGX_ENABLE_SSD1306)
	list(APPEND srcs "src/drivers/ssd1306.c")
endif()

if(CONFIG_DGX_ENABLE_V_BW_SCREEN)
	list(APPEND srcs "src/bw_screen.c")
endif()

if(CONFIG_DGX_ENABLE_VSCREEN)
	list(APPEND srcs "src/drivers/vscreen.c")
endif()

if(CONFIG_DGX_ENABLE_VSCREEN_2H)
	list(APPEND srcs "src/drivers/vscreen_2h.c")
endif()

idf_component_register(
	SRCS "${srcs}"
	REQUIRES driver
	PRIV_REQUIRES esp_lcd
	INCLUDE_DIRS "include"
)
#target_compile_options(${COMPONENT_LIB} PRIVATE -Ofast)
