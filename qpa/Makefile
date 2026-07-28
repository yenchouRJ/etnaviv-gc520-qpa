# Makefile — etnaviv2d QPA plugin + tests
#
# Root cause of "Project has no top-level .qmake.conf": the host
# output/host/bin/qmake is a wrapper that does NOT know the Qt build-tree
# .qmake.conf.  The plugin and tests must be built using the IN-TREE qmake
# at $QT_BUILD/bin/qmake, which embeds all include paths, mkspec, and
# .qmake.conf location.
#
# Build strategy:
#   - Plugin: copied/symlinked into $QT_BUILD/src/plugins/platforms/etnaviv2d/
#     and built in-tree (same as linuxfb, eglfs, etc).
#   - Tests: simpler standalone apps built with in-tree qmake too (they only
#     need QT += gui/widgets, which in-tree qmake resolves correctly).
#
# Prerequisites:
#   make -C ../hal/libetna2d          (build libetna2d.a first)
#
# Targets:
#   make all         — plugin + all tests
#   make plugin      — plugin only (installs into Qt build tree)
#   make tests       — all four step tests
#   make clean       — remove build output (does NOT clean Qt build tree)

QT_BUILD  ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
QMAKE     := $(QT_BUILD)/bin/qmake

ETNA2D_LIBDIR ?= $(abspath $(CURDIR)/../hal/libetna2d/build)
ETNA2D_INCDIR ?= $(abspath $(CURDIR)/../hal/libetna2d)
KMS_INCDIR    ?= $(abspath $(CURDIR)/../hal/kms)

# The plugin lives inside the Qt build tree so in-tree qmake finds .qmake.conf
PLUGIN_INTREE_DIR := $(QT_BUILD)/src/plugins/platforms/etnaviv2d

QMAKE_VARS = \
    "ETNA2D_LIBDIR=$(ETNA2D_LIBDIR)" \
    "ETNA2D_INCDIR=$(ETNA2D_INCDIR)" \
    "KMS_INCDIR=$(KMS_INCDIR)"

# Build directories for tests (standalone, out-of-tree)
BUILD_T_BLIT  = build/test_blitter
BUILD_T_SCR   = build/test_screen
BUILD_T_INTG  = build/test_integration
BUILD_T_BENCH = build/test_benchmark

.PHONY: all plugin tests clean \
        test_blitter test_screen test_integration test_benchmark \
        plugin_intree_sync

all: plugin tests

# -----------------------------------------------------------------------
# Plugin: sync sources into Qt build tree, then build in-tree
# -----------------------------------------------------------------------
plugin: plugin_intree_sync $(PLUGIN_INTREE_DIR)/Makefile
	$(MAKE) -C $(PLUGIN_INTREE_DIR)

# Copy/rsync our src/ into the Qt plugin directory.
# rsync -a ensures only changed files are touched (avoids spurious rebuilds).
plugin_intree_sync:
	mkdir -p $(PLUGIN_INTREE_DIR)
	rsync -a --delete \
	    $(CURDIR)/src/ \
	    $(CURDIR)/etnaviv2d.pro \
	    $(PLUGIN_INTREE_DIR)/
	@echo "[ sync ] sources -> $(PLUGIN_INTREE_DIR)"

# Run in-tree qmake from inside the plugin directory so it finds .qmake.conf
$(PLUGIN_INTREE_DIR)/Makefile: plugin_intree_sync
	cd $(PLUGIN_INTREE_DIR) && $(QMAKE) etnaviv2d.pro $(QMAKE_VARS)

# -----------------------------------------------------------------------
# Tests: built with in-tree qmake (resolves private Qt headers correctly)
# -----------------------------------------------------------------------
tests: test_blitter test_screen test_integration test_benchmark

test_blitter: $(BUILD_T_BLIT)/Makefile
	$(MAKE) -C $(BUILD_T_BLIT)

test_screen: $(BUILD_T_SCR)/Makefile
	$(MAKE) -C $(BUILD_T_SCR)

test_integration: $(BUILD_T_INTG)/Makefile
	$(MAKE) -C $(BUILD_T_INTG)

test_benchmark: $(BUILD_T_BENCH)/Makefile
	$(MAKE) -C $(BUILD_T_BENCH)

$(BUILD_T_BLIT)/Makefile: tests/test_blitter/test_blitter.pro | $(BUILD_T_BLIT)
	cd $(BUILD_T_BLIT) && $(QMAKE) $(CURDIR)/tests/test_blitter/test_blitter.pro \
	    $(QMAKE_VARS)

$(BUILD_T_SCR)/Makefile: tests/test_screen/test_screen.pro | $(BUILD_T_SCR)
	cd $(BUILD_T_SCR) && $(QMAKE) $(CURDIR)/tests/test_screen/test_screen.pro \
	    $(QMAKE_VARS)

$(BUILD_T_INTG)/Makefile: tests/test_integration/test_integration.pro | $(BUILD_T_INTG)
	cd $(BUILD_T_INTG) && $(QMAKE) $(CURDIR)/tests/test_integration/test_integration.pro

$(BUILD_T_BENCH)/Makefile: tests/test_benchmark/test_benchmark.pro | $(BUILD_T_BENCH)
	cd $(BUILD_T_BENCH) && $(QMAKE) $(CURDIR)/tests/test_benchmark/test_benchmark.pro

# -----------------------------------------------------------------------
# Directory creation
# -----------------------------------------------------------------------
$(BUILD_T_BLIT) $(BUILD_T_SCR) $(BUILD_T_INTG) $(BUILD_T_BENCH):
	mkdir -p $@

# -----------------------------------------------------------------------
# Clean: only our build dirs; does NOT touch the Qt build tree
# -----------------------------------------------------------------------
clean:
	rm -rf build
	@echo "Note: to remove in-tree plugin: rm -rf $(PLUGIN_INTREE_DIR)"
