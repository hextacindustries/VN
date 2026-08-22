# Build the VN engine. The core has no dependencies beyond a C99
# compiler and libc; the headless port needs nothing else at all.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=199309L
INCLUDE  = -Icore/include -Idemo/src
BUILD    = build

CORE_SRC = core/src/vn_gfx.c core/src/vn_text.c core/src/vn_vm.c core/src/vn_save.c
PLAT_SRC = platform/headless/vn_plat_headless.c
DEMO_SRC = demo/src/demo_frame.c demo/src/main.c

.PHONY: all clean font script frames test

all: $(BUILD)/vndemo $(BUILD)/vnfont $(BUILD)/vnc

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/vndemo: $(CORE_SRC) $(PLAT_SRC) $(DEMO_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDE) $^ -o $@

$(BUILD)/vnfont: tools/vnfont/vnfont.c | $(BUILD)
	$(CC) -std=c99 -O2 -Wall $< -o $@ -lm

$(BUILD)/vnc: tools/vnc/vnc.c | $(BUILD)
	$(CC) $(CFLAGS) -Icore/include $< -o $@

# Regenerate the bitmap font from a system TTF.
font: $(BUILD)/vnfont
	$(BUILD)/vnfont /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \
	    demo/assets/font8x16.vnf 16 8

script: $(BUILD)/vnc
	$(BUILD)/vnc demo/script/demo.vn $(BUILD)/demo.vnb $(BUILD)/demo.vnstr

frames: $(BUILD)/vndemo script
	mkdir -p $(BUILD)/frames
	$(BUILD)/vndemo demo/assets/font8x16.vnf \
	    $(BUILD)/demo.vnb $(BUILD)/demo.vnstr $(BUILD)/frames

test:
	./tests/run_tests.sh

clean:
	rm -rf $(BUILD)
