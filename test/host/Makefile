# Host tests for the platform-independent parts of DGX.
# Run: make -C test/host

ROOT    := ../..
CC      ?= gcc
CFLAGS  ?= -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# -Wno-format: ESP32 logs print size_t with %u, which is 32-bit only there.
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format
# a small band, for a transfer of the tests to take several of them
CFLAGS  += -DDGX_VSCREEN8_BAND_BYTES=200u
CPPFLAGS += -Istubs -I$(ROOT)/include -I$(ROOT)/src -include stubs/esp_log.h
LDLIBS  += -lm

LIB_SRCS := $(addprefix $(ROOT)/src/, matrix.c morph.c morph_sources.c morph_render.c \
              font.c pixels.c hw_font.c hw_morph.c font_morph.c bitmap.c draw.c bezier.c texture.c orientation.c drivers/vscreen.c \
              bw_screen.c dgx_bus_optimized_funcs.c dgx_spi_area_protocol.c gauge.c bus/fb_area.c bus/fb_tiles.c bus/fb_flip.c) \
            $(wildcard $(ROOT)/src/fonts/*.c)

TESTS := test_fb_area test_fb_tiles test_fb_flip test_pixels test_fill test_circle test_matrix test_morph test_render test_fonts test_screen test_region test_bw test_texture test_bezier test_hw_font test_hw_line test_hw_write test_hw_morph test_soft_pen test_gauge test_oriented test_p8 test_p8_lcdcam test_st7920
BUILD := build

all: $(addprefix run-, $(TESTS))

run-%: $(BUILD)/%
	./$<

$(BUILD)/%: %.c $(LIB_SRCS) check.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_SRCS) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

.SECONDARY:
