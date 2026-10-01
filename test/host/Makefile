# Host tests for the platform-independent parts of DGX.
# Run: make -C test/host

ROOT    := ../..
CC      ?= gcc
CFLAGS  ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
# -Wno-format: ESP32 logs print size_t with %u, which is 32-bit only there.
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format
CPPFLAGS += -Istubs -I$(ROOT)/include -I$(ROOT)/src -include stubs/esp_log.h
LDLIBS  += -lm

LIB_SRCS := $(addprefix $(ROOT)/src/, matrix.c morph.c morph_sources.c morph_render.c \
              font.c bitmap.c draw.c orientation.c drivers/vscreen.c) \
            $(wildcard $(ROOT)/src/fonts/*.c)

TESTS := test_matrix test_morph test_render test_fonts
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
