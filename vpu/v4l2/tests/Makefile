# Build the userspace tests for the Hantro PP work.  Default: cross-compile (Buildroot toolchain).
#   make          -> build/ (aarch64);  scp build/t_* to the board
#   make HOST=1   -> native build (syntax check only)
B ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_graphic
ifeq ($(HOST),1)
CC := gcc
else
CC := $(B)/output/host/bin/aarch64-nuvoton-linux-gnu-gcc
endif
CFLAGS ?= -O2 -g -Wall -Wextra
OUT := build
TESTS := t_pp_fmt

all: $(addprefix $(OUT)/,$(TESTS))
$(OUT)/%: %.c pp_test.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $<
$(OUT):
	mkdir -p $@
.PHONY: all
