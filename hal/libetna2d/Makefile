# Makefile — libetna2d (GC520 2D op builder, Phase 2) + self-test.
#
# Builds the reusable, Qt-independent 2D HAL as both a shared and a static
# library, plus an on-target acceptance test. Cross-compiled for MA35D1.
#
# Build:
#   A) source the Buildroot SDK env, then: make
#   B) make BR=/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
#
# Artifacts (all under build/):
#   build/libetna2d.so.1   build/libetna2d.so   build/libetna2d.a
#   build/etna2d_selftest
#
# Deploy build/libetna2d.so.1 + build/etna2d_selftest to the target and run:
#   ETNA2D_VERBOSE=1 ./etna2d_selftest /dev/dri/renderD128

# ---- Buildroot locations -------------------------------------------------
BR      ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
HOST    ?= $(BR)/output/host
SYSROOT ?= $(HOST)/aarch64-nuvoton-linux-gnu/sysroot
CROSS   ?= $(HOST)/bin/aarch64-nuvoton-linux-gnu-

ifeq ($(origin CC),default)
CC := $(CROSS)gcc
endif
AR      ?= $(CROSS)ar

# libdrm build tree — source of the etnaviv register headers.
LIBDRM  ?= $(BR)/output/build/libdrm-2.4.120
ETNA_H  := $(LIBDRM)/tests/etnaviv

# ---- Output directory ----------------------------------------------------
BUILDDIR := build
$(shell mkdir -p $(BUILDDIR))

# ---- Flags ---------------------------------------------------------------
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=gnu11 -fPIC
CFLAGS  += --sysroot=$(SYSROOT)
CFLAGS  += -I$(SYSROOT)/usr/include/libdrm
CFLAGS  += -I$(ETNA_H)
CFLAGS  += -I$(LIBDRM)/etnaviv
CFLAGS  += -I.

LDFLAGS ?=
LDFLAGS += --sysroot=$(SYSROOT)
LDLIBS  := -ldrm_etnaviv -ldrm

SONAME  := $(BUILDDIR)/libetna2d.so.1
SHARED  := $(BUILDDIR)/libetna2d.so
STATIC  := $(BUILDDIR)/libetna2d.a
OBJ     := $(BUILDDIR)/libetna2d.o

.PHONY: all clean check-env
all: $(SONAME) $(SHARED) $(STATIC) $(BUILDDIR)/etna2d_selftest

$(OBJ): libetna2d.c etna2d.h
	$(CC) $(CFLAGS) -c $< -o $@

# Shared library with a proper SONAME.
$(SONAME): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,libetna2d.so.1 \
		$< $(LDLIBS) -o $@

$(SHARED): $(SONAME)
	ln -sf libetna2d.so.1 $(SHARED)

$(STATIC): $(OBJ)
	$(AR) rcs $@ $<

# Self-test links the static lib for a self-contained on-target binary.
$(BUILDDIR)/etna2d_selftest: etna2d_selftest.c $(STATIC)
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(STATIC) $(LDLIBS) -o $@

check-env:
	@echo "CC      = $(CC)"
	@echo "SYSROOT = $(SYSROOT)"
	@echo "LIBDRM  = $(LIBDRM)"
	@test -x "$(CC)" || { echo "ERROR: CC not found — source SDK env or set BR"; exit 1; }
	@test -f "$(ETNA_H)/state_2d.xml.h" || { echo "ERROR: etnaviv headers not under $(ETNA_H)"; exit 1; }
	@test -f "$(SYSROOT)/usr/lib/libdrm_etnaviv.so" || { echo "ERROR: libdrm_etnaviv not in sysroot"; exit 1; }
	@echo "environment OK"

clean:
	rm -rf $(BUILDDIR)
