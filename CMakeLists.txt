file(GLOB dgx_font_sources
	"${COMPONENT_DIR}/src/fonts/*.c"
)

# The sources by topic: what every screen does (screen/), drawing on any screen (draw/), virtual screens and
# what moves pictures between screens (vscreen/), text (text/), morphing (morph/); the buses (bus/), the
# panels (drivers/) and the fonts (fonts/) are enabled one by one below.
#
# hw_font.c, bezier.c and hw_morph.c stand together: a text is written by the first calling the second for
# every stroke, and the linker puts the code of an archive into flash in the order its files are needed. Code
# that lies 16 KB apart shares the lines of the 2-way flash cache of the ESP32; side by side the two do not.
set(srcs
	"src/drivers/dgx_lcd_init.c"
	"src/screen/screen.c" "src/screen/screen_bus.c" "src/screen/pixels.c" "src/screen/bitmap.c"
	"src/draw/draw.c" "src/draw/lines.c" "src/draw/circles.c" "src/draw/polygons.c"
	"src/text/font.c" "src/text/hw_font.c" "src/draw/bezier.c" "src/text/hw_morph.c" "src/text/font_morph.c"
	"src/draw/gauge.c" "src/draw/texture.c" "src/draw/ellipse.c" "src/draw/scroll.c"
	"src/vscreen/lut.c" "src/vscreen/scale.c" "src/vscreen/overlay.c")

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

if(CONFIG_DGX_ENABLE_JD9365)
	list(APPEND srcs "src/drivers/jd9365.c")
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

if(CONFIG_DGX_ENABLE_SSD1309)
	list(APPEND srcs "src/drivers/ssd1309.c")
endif()

if(CONFIG_DGX_ENABLE_SH1106)
	list(APPEND srcs "src/drivers/sh1106.c")
endif()

if(CONFIG_DGX_ENABLE_V_BW_SCREEN)
	list(APPEND srcs "src/vscreen/bw_screen.c")
endif()

if(CONFIG_DGX_ENABLE_VSCREEN)
	list(APPEND srcs "src/vscreen/vscreen.c" "src/vscreen/transfer.c" "src/vscreen/image.c")
endif()

if(CONFIG_DGX_ENABLE_VSCREEN_2H)
	list(APPEND srcs "src/vscreen/vscreen_2h.c")
endif()

if(CONFIG_DGX_ENABLE_MORPH)
	list(APPEND srcs "src/morph/matrix.c" "src/morph/morph.c" "src/morph/morph_sources.c")
	if(CONFIG_DGX_ENABLE_VSCREEN)
		list(APPEND srcs "src/morph/morph_render.c")
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

# Code kept in IRAM by request, past the flash cache: see docs/pen-en.md. A linker fragment has to name the
# archive, and the archive is named after the component, that is after its directory or by the component
# manager; so the fragment is written here.
set(dgx_ldfragments "")
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND (CONFIG_DGX_HW_FONT_IN_IRAM OR CONFIG_DGX_BEZIER_IN_IRAM))
	set(dgx_iram_entries "")
	if(CONFIG_DGX_HW_FONT_IN_IRAM)
		string(APPEND dgx_iram_entries "    hw_font (noflash_text)\n")
		string(APPEND dgx_iram_entries "    font:dgx_font_find_glyph (noflash_text)\n")
		string(APPEND dgx_iram_entries "    font:decodeUTF8next (noflash_text)\n")
	endif()
	if(CONFIG_DGX_BEZIER_IN_IRAM)
		string(APPEND dgx_iram_entries "    bezier (noflash_text)\n")
	endif()
	set(dgx_ldfragments "${CMAKE_CURRENT_BINARY_DIR}/dgx_iram.lf")
	file(WRITE "${dgx_ldfragments}" "[mapping:dgx_iram]\narchive: lib${COMPONENT_NAME}.a\nentries:\n${dgx_iram_entries}")
endif()

idf_component_register(
	SRCS "${srcs}"
	REQUIRES ${dgx_requires}
	PRIV_REQUIRES ${dgx_priv_requires}
	INCLUDE_DIRS "include"
	LDFRAGMENTS ${dgx_ldfragments}
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
