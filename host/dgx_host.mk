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
# lines, circles, polygons, ellipses and arcs, Bezier curves and the pen, textures, scrolling, the gauge
DGX_HOST_SRCS_DRAW    := $(addprefix $(DGX_ROOT)/src/draw/, draw.c lines.c circles.c polygons.c ellipse.c bezier.c texture.c \
                           scroll.c gauge.c)
# virtual screens and their transfers, scaling and smoothing, the overlay, sprites, tables of colors, the page screen
DGX_HOST_SRCS_VSCREEN := $(addprefix $(DGX_ROOT)/src/vscreen/, vscreen.c transfer.c bw_screen.c image.c lut.c scale.c overlay.c)
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
