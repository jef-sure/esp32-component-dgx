# Host tests for the platform-independent parts of DGX.
# Run: make -C test/host

ROOT    := ../..
include $(ROOT)/host/dgx_host.mk
CC      ?= gcc
CFLAGS  ?= -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# -Wno-format: ESP32 logs print size_t with %u, which is 32-bit only there.
CFLAGS  += $(DGX_HOST_CFLAGS) -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format
# the tests include private headers of src/ as well
CPPFLAGS += $(DGX_HOST_CPPFLAGS) -I$(ROOT)/src
LDLIBS  += $(DGX_HOST_LDLIBS)

LIB_SRCS := $(DGX_HOST_SRCS)

TESTS := test_fb_area test_fb_tiles test_fb_flip test_pixels test_fill test_circle test_ellipse test_scroll test_panel_scroll test_matrix test_morph test_render test_fonts test_screen test_region test_bw test_texture test_bezier test_hw_font test_hw_line test_hw_write test_hw_morph test_soft_pen test_gauge test_oriented test_p8 test_p8_lcdcam test_st7920 test_ssd1309 test_sh1106 test_image test_lut test_overlay test_overlay_whole_rows test_frame_to_panel test_life_panel test_scale test_transfer test_polygon test_ssd1351 test_bus_owner test_argb
BUILD := build

all: $(addprefix run-, $(TESTS))

run-%: $(BUILD)/%
	./$<

# the headers, and the drivers and buses some tests include as they are: a change of any of them builds the tests again
DEPS := $(wildcard $(ROOT)/include/*.h $(ROOT)/include/*/*.h $(ROOT)/src/*/*.h $(ROOT)/src/drivers/*.c $(ROOT)/src/bus/*.c) $(wildcard *.h $(DGX_HOST_DIR)/stubs/*.h $(DGX_HOST_DIR)/stubs/*/*.h) $(DGX_HOST_DIR)/dgx_host.mk

$(BUILD)/%: %.c $(LIB_SRCS) $(DEPS) check.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_SRCS) $(LDLIBS) -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean

.SECONDARY:
