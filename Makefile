# Build the VN engine. The core has no dependencies beyond a C99
# compiler and libc; the headless port needs nothing else at all.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=199309L
INCLUDE  = -Icore/include -Idemo/src
BUILD    = build

CORE_SRC = core/src/vn_gfx.c core/src/vn_text.c
PLAT_SRC = platform/headless/vn_plat_headless.c
DEMO_SRC = demo/src/demo_frame.c demo/src/main.c

.PHONY: all clean demo font frames

all: $(BUILD)/vndemo $(BUILD)/vnfont

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/vndemo: $(CORE_SRC) $(PLAT_SRC) $(DEMO_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDE) $^ -o $@

$(BUILD)/vnfont: tools/vnfont/vnfont.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall $< -o $@ -lm

# Regenerate the bitmap font from a system TTF.
font: $(BUILD)/vnfont
	$(BUILD)/vnfont /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
	    demo/assets/font8x16.vnf 16 8

frames: $(BUILD)/vndemo
	mkdir -p $(BUILD)/frames
	$(BUILD)/vndemo demo/assets/font8x16.vnf $(BUILD)/frames

clean:
	rm -rf $(BUILD)
