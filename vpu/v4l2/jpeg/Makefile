# Userspace tests for the Hantro JPEG decoder.  Default: cross-compile (Buildroot toolchain).
#   make          -> build/t_jpeg_dec (aarch64);  scp to the board
#   make HOST=1   -> native build (syntax check only)
B ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_graphic
ifeq ($(HOST),1)
CC := gcc
else
CC := $(B)/output/host/bin/aarch64-nuvoton-linux-gnu-gcc
endif
CFLAGS ?= -O2 -g -Wall -Wextra
OUT := build

all: $(OUT)/t_jpeg_dec
$(OUT)/%: %.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<
$(OUT):
	mkdir -p $@
.PHONY: all
