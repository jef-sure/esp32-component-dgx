# morph_demo on the computer: main/main.c as it is, in a window of SDL2 in place of the CYD (host/window of DGX).
# make, then make run (or ./build-host/morph_demo); make clean. Needs SDL2 (sdl2-config) and a shell without ESP-IDF
# activated: its linker does not link programs for the computer. DGX=<dgx> takes another checkout of DGX.
DGX   ?= ../..
BUILD := build-host
PROG  := $(BUILD)/morph_demo

include $(DGX)/host/dgx_host.mk

ifneq ($(findstring .espressif,$(shell command -v ld)),)
$(error ld is ESP-IDF's ($(shell command -v ld)): build for the computer in a shell without ESP-IDF activated)
endif

CFLAGS   ?= -O2 -g
CFLAGS   += $(DGX_HOST_CFLAGS) -MMD -MP
CPPFLAGS += $(DGX_HOST_CPPFLAGS) $(DGX_HOST_WINDOW_CPPFLAGS)
LDLIBS   += $(DGX_HOST_LDLIBS) $(DGX_HOST_WINDOW_LDLIBS)

# an object of every source under build-host/, where the source lies
SRCS := $(abspath main/main.c) $(DGX_HOST_SRCS) $(DGX_HOST_WINDOW_SRCS)
OBJS := $(patsubst /%.c,$(BUILD)/obj/%.o,$(SRCS))

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/obj/%.o: /%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

run: $(PROG)
	./$(PROG)

clean:
	rm -rf $(BUILD)

.PHONY: run clean

-include $(OBJS:.o=.d)
