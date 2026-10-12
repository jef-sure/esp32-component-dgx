file(GLOB dgx_font_sources
	"${COMPONENT_DIR}/src/fonts/*.c"
)

set(srcs "src/drivers/dgx_lcd_init.c" "src/dgx_bus_optimized_funcs.c" "src/dgx_spi_area_protocol.c" "src/draw.c" "src/pixels.c" "src/bezier.c" "src/font.c" "src/hw_font.c" "src/hw_morph.c" "src/font_morph.c" "src/orientation.c" "src/bitmap.c" "src/gauge.c" "src/texture.c")

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

if(CONFIG_DGX_ENABLE_DSI)
	list(APPEND srcs "src/bus/dsi_esp32.c" "src/bus/fb_area.c" "src/bus/fb_tiles.c" "src/bus/fb_flip.c")
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

if(CONFIG_DGX_ENABLE_JD9165)
	list(APPEND srcs "src/drivers/jd9165.c")
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

if(CONFIG_DGX_ENABLE_MORPH)
	list(APPEND srcs "src/matrix.c" "src/morph.c" "src/morph_sources.c")
	if(CONFIG_DGX_ENABLE_VSCREEN)
		list(APPEND srcs "src/morph_render.c")
	endif()
endif()

# IDF 5.3 split "driver" into esp_driver_*; since IDF 6 "driver" no longer re-exports them.
if(IDF_VERSION_MAJOR GREATER 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
	set(dgx_requires esp_driver_gpio esp_driver_spi esp_driver_i2c)
else()
	set(dgx_requires driver)
endif()

# the 2D DMA that copies between the frame buffers of a DSI display has its own component since IDF 6
set(dgx_priv_requires esp_lcd esp_mm esp_timer)
if(IDF_VERSION_MAJOR GREATER 5)
	list(APPEND dgx_priv_requires esp_driver_dma)
endif()
# the accelerator that fills rectangles of a DSI display, a component since IDF 5.3
if(IDF_VERSION_MAJOR GREATER 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
	list(APPEND dgx_priv_requires esp_driver_ppa)
endif()

idf_component_register(
	SRCS "${srcs}"
	REQUIRES ${dgx_requires}
	PRIV_REQUIRES ${dgx_priv_requires}
	INCLUDE_DIRS "include"
)

# The driver of a DSI display got a new name for the moment a frame buffer is free, in IDF 5.5.5, 6.0.3 and 6.1:
# told by the header, for the release branches got it at different times.
if(CONFIG_DGX_ENABLE_DSI)
	idf_build_get_property(dgx_idf_path IDF_PATH)
	set(dgx_dsi_header "${dgx_idf_path}/components/esp_lcd/dsi/include/esp_lcd_mipi_dsi.h")
	if(EXISTS "${dgx_dsi_header}")
		file(STRINGS "${dgx_dsi_header}" dgx_dsi_frame_buf_complete REGEX "on_frame_buf_complete")
		if(dgx_dsi_frame_buf_complete)
			target_compile_definitions(${COMPONENT_LIB} PRIVATE DGX_DSI_FRAME_BUF_COMPLETE=1)
		endif()
	endif()
endif()
#target_compile_options(${COMPONENT_LIB} PRIVATE -Ofast)
