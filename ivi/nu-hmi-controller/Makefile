# Makefile — nu-hmi-controller.so custom ivi-shell HMI controller
#
# Build:
#   source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup
#   make
#
# Deploy:
#   scp build/nu-hmi-controller.so root@192.168.0.102:/usr/lib/weston/
#
# The plugin is loaded by Weston via weston.ini:
#   [core]
#   modules=nu-hmi-controller.so

SYSROOT ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/aarch64-nuvoton-linux-gnu/sysroot

BUILD_DIR = build

WESTON_BUILD = /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/build/weston-10.0.5

CFLAGS += \
    -Wall -Wextra \
    -fPIC \
    -I$(SYSROOT)/usr/include \
    -I$(SYSROOT)/usr/include/pixman-1 \
    -I$(SYSROOT)/usr/include/libweston-10 \
    -I$(SYSROOT)/usr/include/weston \
    -I$(WESTON_BUILD) \
    -DHAVE_CONFIG_H

# config.h lives in the weston build tree
CFLAGS += -I$(WESTON_BUILD)/build

LDFLAGS += \
    -shared \
    -L$(SYSROOT)/usr/lib \
    -Wl,-rpath-link,$(SYSROOT)/usr/lib \
    -lweston-10 \
    -lwayland-server

TARGET = $(BUILD_DIR)/nu-hmi-controller.so
SRC    = nu-hmi-controller.c

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)
