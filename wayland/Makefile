# Makefile — wayland/ Phase 7 test harnesses
#
# Mirrors compositor/Makefile structure.
#
# Prerequisites:
#   source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup
#   (plugin must already be built: cd qpa && make plugin)
#
# Targets:
#   make all              — all test harnesses
#   make qt_ui_client     — Milestone B: minimal Qt wayland client
#   make clean            — remove build output (does NOT need permission — no logs here)

QT_BUILD ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
QMAKE    := $(QT_BUILD)/bin/qmake

BUILD_QT_UI = build/qt_ui_client

.PHONY: all qt_ui_client clean

all: qt_ui_client

qt_ui_client: $(BUILD_QT_UI)/Makefile
	$(MAKE) -C $(BUILD_QT_UI)

$(BUILD_QT_UI)/Makefile: tests/qt_ui_client/qt_ui_client.pro | $(BUILD_QT_UI)
	cd $(BUILD_QT_UI) && $(QMAKE) $(CURDIR)/tests/qt_ui_client/qt_ui_client.pro

$(BUILD_QT_UI):
	mkdir -p $@

clean:
	rm -rf build
