# Makefile — compositor tests
#
# Mirrors the pattern used by qpa/Makefile:
#   - Tests are standalone Qt apps built with in-tree qmake.
#   - No plugin involved; tests link only Qt widgets.
#
# Prerequisites:
#   None beyond Qt build tree (no libetna2d needed here).
#
# Targets:
#   make all               — all tests
#   make test_alpha_blend  — Step 3 overlay alpha blend verification
#   make test_composite    — Step 4 single-committer composite mode
#   make clean             — remove build output

QT_BUILD ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
QMAKE    := $(QT_BUILD)/bin/qmake

BUILD_T_ALPHA     = build/test_alpha_blend
BUILD_T_COMPOSITE = build/test_composite

.PHONY: all test_alpha_blend test_composite clean

all: test_alpha_blend test_composite

test_alpha_blend: $(BUILD_T_ALPHA)/Makefile
	$(MAKE) -C $(BUILD_T_ALPHA)

$(BUILD_T_ALPHA)/Makefile: tests/test_alpha_blend/test_alpha_blend.pro | $(BUILD_T_ALPHA)
	cd $(BUILD_T_ALPHA) && $(QMAKE) $(CURDIR)/tests/test_alpha_blend/test_alpha_blend.pro

$(BUILD_T_ALPHA):
	mkdir -p $@

test_composite: $(BUILD_T_COMPOSITE)/Makefile
	$(MAKE) -C $(BUILD_T_COMPOSITE)

$(BUILD_T_COMPOSITE)/Makefile: tests/test_composite/test_composite.pro | $(BUILD_T_COMPOSITE)
	cd $(BUILD_T_COMPOSITE) && $(QMAKE) $(CURDIR)/tests/test_composite/test_composite.pro

$(BUILD_T_COMPOSITE):
	mkdir -p $@

clean:
	rm -rf build
