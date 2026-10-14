# Build the userspace tests.  Default: cross-compile with the Buildroot toolchain.
#   make            -> build/ (aarch64)
#   make HOST=1     -> native build (syntax check only; needs libdrm headers)
B ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_graphic
ifeq ($(HOST),1)
CC := gcc
LIBDRM_INC := $(shell pkg-config --cflags-only-I libdrm 2>/dev/null)
else
CC := $(B)/output/host/bin/aarch64-nuvoton-linux-gnu-gcc
LIBDRM_INC := -I$(B)/output/staging/usr/include/libdrm
endif
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-function
CFLAGS += -I../include/uapi $(LIBDRM_INC)
OUT := build
TESTS := t_probe t_gem t_job t_replay

all: $(addprefix $(OUT)/,$(TESTS))
$(OUT)/%: %.c vc8k_test.h ../include/uapi/drm/vc8k_drm.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<
$(OUT):
	mkdir -p $@
.PHONY: all
