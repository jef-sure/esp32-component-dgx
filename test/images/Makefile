# Pictures of PNG files made into .dgxi files (tools/dgx_images.py), read and drawn by DGX on the host: make -C test/images
ROOT := ../..
include $(ROOT)/host/dgx_host.mk
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
# -Wno-format: ESP32 logs print size_t with %u, which is 32-bit only there.
CFLAGS += $(DGX_HOST_CFLAGS) -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format
BUILD  := build

all: $(BUILD)/dgxi_dump
	python3 test_images.py $(BUILD)/dgxi_dump

$(BUILD)/dgxi_dump: dgxi_dump.c $(DGX_HOST_SRCS) $(wildcard $(ROOT)/include/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(DGX_HOST_CPPFLAGS) -o $@ dgxi_dump.c $(DGX_HOST_SRCS) $(DGX_HOST_LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
