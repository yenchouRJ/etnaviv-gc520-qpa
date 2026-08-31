# Makefile — wayland/ Phase 7 test harnesses
#
# Targets:
#   make all                  — all test harnesses
#   make qt_ui_client         — Milestone B: minimal Qt wayland client
#   make gst_dmabuf_video     — Milestone A: NV12 DMABuf GStreamer video client (standalone)
#   make gst_plugin_dmabuf_src— dmabufsrc GStreamer plugin (.so) for gst-launch-1.0
#   make clean                — remove build output

QT_BUILD ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/qt5base-bebdfd54917e25d1c100e6bd9f5dd53c2e645fd8
QMAKE    := $(QT_BUILD)/bin/qmake

SYSROOT  ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/aarch64-nuvoton-linux-gnu/sysroot

BUILD_QT_UI   = build/qt_ui_client
BUILD_GST     = build/gst_dmabuf_video
BUILD_PLUGIN  = build/gst_plugin_dmabuf_src

.PHONY: all qt_ui_client gst_dmabuf_video gst_plugin_dmabuf_src clean

all: qt_ui_client gst_dmabuf_video gst_plugin_dmabuf_src

# ── Qt UI client (Milestone B) ─────────────────────────────────────
qt_ui_client: $(BUILD_QT_UI)/Makefile
	$(MAKE) -C $(BUILD_QT_UI)

$(BUILD_QT_UI)/Makefile: tests/qt_ui_client/qt_ui_client.pro | $(BUILD_QT_UI)
	cd $(BUILD_QT_UI) && $(QMAKE) $(CURDIR)/tests/qt_ui_client/qt_ui_client.pro

$(BUILD_QT_UI):
	mkdir -p $@

# ── GStreamer DMABuf standalone video client (Milestone A) ─────────
GST_CFLAGS := $(shell PKG_CONFIG_PATH=$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/lib64/pkgconfig \
                  pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 libdrm 2>/dev/null)
GST_LIBS   := $(shell PKG_CONFIG_PATH=$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/lib64/pkgconfig \
                  pkg-config --libs   gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 libdrm 2>/dev/null)

gst_dmabuf_video: $(BUILD_GST)/gst_dmabuf_video

$(BUILD_GST)/gst_dmabuf_video: tests/gst_dmabuf_video/gst_dmabuf_video.c | $(BUILD_GST)
	$(CC) $(CFLAGS) $(GST_CFLAGS) -o $@ $< $(GST_LIBS)

$(BUILD_GST):
	mkdir -p $@

# ── dmabufsrc GStreamer plugin — use with gst-launch-1.0 ───────────
PLUGIN_CFLAGS := $(shell PKG_CONFIG_PATH=$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/lib64/pkgconfig \
                     pkg-config --cflags gstreamer-1.0 gstreamer-base-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0 libdrm 2>/dev/null)
PLUGIN_LIBS   := $(shell PKG_CONFIG_PATH=$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/lib64/pkgconfig \
                     pkg-config --libs   gstreamer-1.0 gstreamer-base-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0 libdrm 2>/dev/null)

gst_plugin_dmabuf_src: $(BUILD_PLUGIN)/libgstdmabufsrc.so

$(BUILD_PLUGIN)/libgstdmabufsrc.so: tests/gst_plugin_dmabuf_src/gstdmabufsrc.c | $(BUILD_PLUGIN)
	$(CC) $(CFLAGS) $(PLUGIN_CFLAGS) -DPACKAGE=\"gstdmabufsrc\" -shared -fPIC -o $@ $< $(PLUGIN_LIBS)

$(BUILD_PLUGIN):
	mkdir -p $@

clean:
	rm -rf build
