# The platform-independent part of DGX built on a PC: include this file from a Makefile.
#
#   include path/to/dgx/host/dgx_host.mk
#   CPPFLAGS += $(DGX_HOST_CPPFLAGS)
#   CFLAGS   += $(DGX_HOST_CFLAGS)
#   LDLIBS   += $(DGX_HOST_LDLIBS)
#   game: main.c $(DGX_HOST_SRCS)
#
# DGX_HOST_SRCS is every group below; take the groups one by one to build less. The paths are absolute,
# found from where this file lies, so the including Makefile may sit anywhere. host/stubs stand in for the
# few ESP-IDF headers these sources include: logging, heap capabilities, error codes, FreeRTOS, GPIO.
# The host tests of DGX, test/host/Makefile, are built from this file too.

DGX_HOST_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
DGX_ROOT     := $(patsubst %/,%,$(dir $(DGX_HOST_DIR)))

# what every screen does: the window of a display behind a bus, pixel runs, 1-bit bitmaps
DGX_HOST_SRCS_SCREEN  := $(addprefix $(DGX_ROOT)/src/screen/, screen.c screen_bus.c pixels.c bitmap.c)
# lines, circles, polygons, ellipses and arcs, Bezier curves and straight segments by the pen, textures, scrolling, the gauge
DGX_HOST_SRCS_DRAW    := $(addprefix $(DGX_ROOT)/src/draw/, draw.c lines.c circles.c polygons.c ellipse.c bezier.c segment.c texture.c \
                           scroll.c gauge.c rectangles.c)
# virtual screens and their transfers, scaling and smoothing, the overlay, sprites, tables of colors, the page screen
DGX_HOST_SRCS_VSCREEN := $(addprefix $(DGX_ROOT)/src/vscreen/, vscreen.c transfer.c alpha.c bw_screen.c image.c lut.c scale.c overlay.c)
# bitmap and dot fonts, handwritten text and its morphs
DGX_HOST_SRCS_TEXT    := $(addprefix $(DGX_ROOT)/src/text/, font.c font_morph.c hw_font.c hw_morph.c)
# the dot morphing framework and its renderers
DGX_HOST_SRCS_MORPH   := $(addprefix $(DGX_ROOT)/src/morph/, matrix.c morph.c morph_sources.c morph_render.c)
# the frame buffer of a DSI display: dirty areas, tiles, the flip of two buffers
DGX_HOST_SRCS_FB      := $(addprefix $(DGX_ROOT)/src/bus/, fb_area.c fb_tiles.c fb_flip.c)
# the fonts DGX ships
DGX_HOST_SRCS_FONTS   := $(wildcard $(DGX_ROOT)/src/fonts/*.c)

DGX_HOST_SRCS := $(DGX_HOST_SRCS_SCREEN) $(DGX_HOST_SRCS_DRAW) $(DGX_HOST_SRCS_VSCREEN) $(DGX_HOST_SRCS_TEXT) \
                 $(DGX_HOST_SRCS_MORPH) $(DGX_HOST_SRCS_FB) $(DGX_HOST_SRCS_FONTS)

DGX_HOST_CPPFLAGS := -I$(DGX_HOST_DIR)/stubs -I$(DGX_ROOT)/include -include $(DGX_HOST_DIR)/stubs/esp_log.h
DGX_HOST_CFLAGS   := -std=gnu11
DGX_HOST_LDLIBS   := -lm

# A window of SDL2 on the computer in place of the display of a board, for a program written for the board:
# host/window/dgx_host_screen.h. Added to the above:
#
#   CPPFLAGS += $(DGX_HOST_WINDOW_CPPFLAGS)
#   LDLIBS   += $(DGX_HOST_WINDOW_LDLIBS)
#   effect: main.c $(DGX_HOST_SRCS) $(DGX_HOST_WINDOW_SRCS)
#
# DGX_HOST_WINDOW is defined in every file then, and vTaskDelay() and dgx_delay() wait. The linker sends malloc,
# calloc, realloc and free of the program and of DGX through the window, which counts them. Needs sdl2-config.
DGX_HOST_WINDOW_SRCS     := $(DGX_HOST_DIR)/window/host_screen.c $(addprefix $(DGX_ROOT)/src/drivers/, panel.c dgx_lcd_init.c)
# the drivers take their pins and waits from dgx_arch_esp32.h on the chip, from the stub of driver/gpio.h here
DGX_HOST_WINDOW_CPPFLAGS  = -DDGX_HOST_WINDOW -I$(DGX_HOST_DIR)/window -I$(DGX_HOST_DIR)/window/stubs \
                            -include $(DGX_HOST_DIR)/stubs/driver/gpio.h $(shell sdl2-config --cflags)
DGX_HOST_WINDOW_LDLIBS    = $(shell sdl2-config --libs) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
