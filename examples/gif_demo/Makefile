# gif_demo on the computer: main/main.c as it is, in a window of SDL2 in place of the CYD (host/window of DGX), its
# GIFs made into files of pictures by tools/dgx_images.py as the build for the board makes them. make, then make run
# (or ./build-host/gif_demo); make clean. Needs SDL2 (sdl2-config), Python 3, and a shell without ESP-IDF activated:
# its linker does not link programs for the computer. DGX=<dgx> takes another checkout of DGX.
DGX    ?= ../..
BUILD  := build-host
PROG   := $(BUILD)/gif_demo
IMAGES := $(BUILD)/dgx_images

include $(DGX)/host/dgx_host.mk

ifneq ($(findstring .espressif,$(shell command -v ld)),)
$(error ld is ESP-IDF's ($(shell command -v ld)): build for the computer in a shell without ESP-IDF activated)
endif

CFLAGS   ?= -O2 -g
CFLAGS   += $(DGX_HOST_CFLAGS) -MMD -MP
CPPFLAGS += $(DGX_HOST_CPPFLAGS) $(DGX_HOST_WINDOW_CPPFLAGS) -I$(IMAGES)
LDLIBS   += $(DGX_HOST_LDLIBS) $(DGX_HOST_WINDOW_LDLIBS)

# an object of every source under build-host/, where the source lies
SRCS := $(abspath main/main.c) $(DGX_HOST_SRCS) $(DGX_HOST_WINDOW_SRCS)
OBJS := $(patsubst /%.c,$(BUILD)/obj/%.o,$(SRCS))

$(PROG): $(OBJS) $(IMAGES)/dgx_assets.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# the files of pictures of main/images.txt, and dgx_assets.S and .h that embed them, as the build for the board makes them
$(IMAGES)/dgx_assets.S $(IMAGES)/dgx_assets.h: main/images.txt $(wildcard main/*.gif) $(DGX)/tools/dgx_images.py
	@mkdir -p $(IMAGES)
	cd main && python3 $(abspath $(DGX))/tools/dgx_images.py build images.txt $(abspath $(IMAGES))

$(IMAGES)/dgx_assets.o: $(IMAGES)/dgx_assets.S
	$(CC) -c $< -o $@

$(BUILD)/obj/%.o: /%.c $(IMAGES)/dgx_assets.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: $(PROG)
	./$(PROG)

clean:
	rm -rf $(BUILD)

.PHONY: run clean

-include $(OBJS:.o=.d)
