# Makefile — GC520 Phase 1 DE validation tests (cross-compiled for MA35D1)
#
# Two ways to build:
#
#   A) Source the Buildroot SDK env first (recommended), then plain make:
#        . /path/to/ma35d1_br24_qt5/output/host/environment-setup
#        make
#      The env exports CC and the sysroot; this Makefile picks them up and
#      only needs BR to locate the etnaviv register headers (xml.h).
#
#   B) No env: point BR at the Buildroot tree and everything is derived:
#        make BR=/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
#
# Deploy the resulting binaries to the target and run against the etnaviv
# render node, e.g.:
#        ./etnaviv_bitblt_test /dev/dri/renderD128
#        ETNA2D_VERBOSE=1 ./etnaviv_bitblt_test /dev/dri/renderD128 blit.bmp
#        ETNA2D_VERBOSE=1 ./etnaviv_alpha_test /dev/dri/renderD128 alpha.bmp
#        ETNA2D_VERBOSE=1 ./etnaviv_stretch_test /dev/dri/renderD128 stretch.bmp

# ---- Buildroot locations -------------------------------------------------
BR      ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
HOST    ?= $(BR)/output/host
SYSROOT ?= $(HOST)/aarch64-nuvoton-linux-gnu/sysroot
CROSS   ?= $(HOST)/bin/aarch64-nuvoton-linux-gnu-

# Honour CC from the sourced SDK env; otherwise derive from CROSS.
ifeq ($(origin CC),default)
CC := $(CROSS)gcc
endif

# libdrm build tree — source of the etnaviv register headers (state*.xml.h,
# cmdstream.xml.h, write_bmp.[ch]). These are generated in-tree and not
# installed to the sysroot, so reference them directly.
LIBDRM  ?= $(BR)/output/build/libdrm-2.4.120
ETNA_H  := $(LIBDRM)/tests/etnaviv

# ---- Flags ---------------------------------------------------------------
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=gnu11
CFLAGS  += --sysroot=$(SYSROOT)
CFLAGS  += -I$(SYSROOT)/usr/include/libdrm
CFLAGS  += -I$(ETNA_H)
CFLAGS  += -I$(LIBDRM)/etnaviv
CFLAGS  += -I.

LDFLAGS ?=
LDFLAGS += --sysroot=$(SYSROOT)
LDLIBS  := -ldrm_etnaviv -ldrm

# ---- Sources -------------------------------------------------------------
TESTS   := etnaviv_bitblt_test \
           etnaviv_alpha_test \
           etnaviv_stretch_test

.PHONY: all clean check-env
all: $(TESTS)

# write_bmp.o lives in the (possibly read-only) libdrm tree; build a local copy.
write_bmp.o: $(ETNA_H)/write_bmp.c
	$(CC) $(CFLAGS) -c $< -o $@

etna2d_util.o: etna2d_util.c etna2d_util.h
	$(CC) $(CFLAGS) -c $< -o $@

# Each test = its object + shared util + write_bmp.
etnaviv_%_test: etnaviv_%_test.c etna2d_util.o write_bmp.o etna2d_util.h
	$(CC) $(CFLAGS) $(LDFLAGS) $< etna2d_util.o write_bmp.o $(LDLIBS) -o $@

check-env:
	@echo "CC       = $(CC)"
	@echo "SYSROOT  = $(SYSROOT)"
	@echo "LIBDRM   = $(LIBDRM)"
	@test -x "$(CC)" || { echo "ERROR: CC not found — source the SDK env or set BR"; exit 1; }
	@test -f "$(ETNA_H)/state_2d.xml.h" || { echo "ERROR: etnaviv headers not found under $(ETNA_H) — set LIBDRM/BR"; exit 1; }
	@test -f "$(SYSROOT)/usr/lib/libdrm_etnaviv.so" || { echo "ERROR: libdrm_etnaviv not in sysroot — set SYSROOT/BR"; exit 1; }
	@echo "environment OK"

clean:
	rm -f $(TESTS) *.o
