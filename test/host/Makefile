# Host tests for the platform-independent parts of DGX.
# Run: make -C test/host

ROOT    := ../..
CC      ?= gcc
CFLAGS  ?= -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# -Wno-format: ESP32 logs print size_t with %u, which is 32-bit only there.
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format
CPPFLAGS += -Istubs -I$(ROOT)/include -I$(ROOT)/src -include stubs/esp_log.h
LDLIBS  += -lm

LIB_SRCS := $(addprefix $(ROOT)/src/, screen/screen.c screen/screen_bus.c screen/pixels.c screen/bitmap.c \
              draw/draw.c draw/lines.c draw/circles.c draw/polygons.c draw/ellipse.c draw/bezier.c draw/texture.c draw/scroll.c draw/gauge.c \
              vscreen/vscreen.c vscreen/transfer.c vscreen/bw_screen.c vscreen/image.c vscreen/lut.c vscreen/scale.c vscreen/overlay.c \
              text/font.c text/font_morph.c text/hw_font.c text/hw_morph.c \
              morph/matrix.c morph/morph.c morph/morph_sources.c morph/morph_render.c \
              bus/fb_area.c bus/fb_tiles.c bus/fb_flip.c) \
            $(wildcard $(ROOT)/src/fonts/*.c)

TESTS := test_fb_area test_fb_tiles test_fb_flip test_pixels test_fill test_circle test_ellipse test_scroll test_panel_scroll test_matrix test_morph test_render test_fonts test_screen test_region test_bw test_texture test_bezier test_hw_font test_hw_line test_hw_write test_hw_morph test_soft_pen test_gauge test_oriented test_p8 test_p8_lcdcam test_st7920 test_ssd1309 test_sh1106 test_image test_lut test_overlay test_scale test_transfer test_polygon test_ssd1351
BUILD := build

all: $(addprefix run-, $(TESTS))

run-%: $(BUILD)/%
	./$<

# the headers, and the drivers and buses some tests include as they are: a change of any of them builds the tests again
DEPS := $(wildcard $(ROOT)/include/*.h $(ROOT)/include/*/*.h $(ROOT)/src/*/*.h $(ROOT)/src/drivers/*.c $(ROOT)/src/bus/*.c) $(wildcard *.h stubs/*.h stubs/*/*.h)

$(BUILD)/%: %.c $(LIB_SRCS) $(DEPS) check.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_SRCS) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

.SECONDARY:
