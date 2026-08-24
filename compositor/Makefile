# Makefile — compositor tests
#
# Mirrors the pattern used by qpa/Makefile:
#   - Qt tests (test_alpha_blend, test_composite) are built with in-tree qmake.
#   - test_weston is a bare-C Wayland client built with its own Makefile;
#     it requires the buildroot environment-setup to be sourced for CC and
#     pkg-config to resolve wayland-client / libdrm.
#
# Prerequisites:
#   source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup
#
# Targets:
#   make all               — all tests
#   make test_alpha_blend  — Step 3 overlay alpha blend verification
#   make test_composite    — Step 4 single-committer composite mode
#   make test_weston       — Step 6 Weston linux-dmabuf-v1 plane-promotion gate
#   make clean             — remove build output

QT_BUILD ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
QMAKE    := $(QT_BUILD)/bin/qmake

BUILD_T_ALPHA     = build/test_alpha_blend
BUILD_T_COMPOSITE = build/test_composite
BUILD_T_WESTON    = build/test_weston

.PHONY: all test_alpha_blend test_composite test_weston clean

all: test_alpha_blend test_composite test_weston

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

# test_weston: bare-C Wayland client; built out-of-tree into build/test_weston/
# mirroring the qmake out-of-tree pattern used by test_composite.
test_weston: $(BUILD_T_WESTON)
	$(MAKE) -C $(BUILD_T_WESTON) \
	        -f $(CURDIR)/tests/test_weston/Makefile \
	        SRCDIR=$(CURDIR)/tests/test_weston

$(BUILD_T_WESTON):
	mkdir -p $@

clean:
	rm -rf build
