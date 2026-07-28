# Makefile — GC520 Phase 1 DE validation tests (cross-compiled for MA35D1)
#
# Two ways to build:
#   A) Source the Buildroot SDK env first (recommended), then plain make:
#        . /path/to/ma35d1_br24_qt5/output/host/environment-setup
#        make
#   B) No env: point BR at the Buildroot tree:
#        make BR=/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
#
# Artifacts are placed under build/.
# Deploy to the target and run:
#   ./etnaviv_bitblt_test /dev/dri/renderD128
#   ETNA2D_VERBOSE=1 ./etnaviv_bitblt_test /dev/dri/renderD128 blit.bmp
#   ETNA2D_VERBOSE=1 ./etnaviv_alpha_test /dev/dri/renderD128 alpha.bmp
#   ETNA2D_VERBOSE=1 ./etnaviv_stretch_test /dev/dri/renderD128 stretch.bmp

# ---- Buildroot locations -------------------------------------------------
BR      ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
HOST    ?= $(BR)/output/host
SYSROOT ?= $(HOST)/aarch64-nuvoton-linux-gnu/sysroot
CROSS   ?= $(HOST)/bin/aarch64-nuvoton-linux-gnu-

ifeq ($(origin CC),default)
CC := $(CROSS)gcc
endif

LIBDRM  ?= $(BR)/output/build/libdrm-2.4.120
ETNA_H  := $(LIBDRM)/tests/etnaviv

# ---- Output directory ----------------------------------------------------
BUILDDIR := build
$(shell mkdir -p $(BUILDDIR))

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
TESTS := $(addprefix $(BUILDDIR)/, \
    etnaviv_bitblt_test \
    etnaviv_alpha_test \
    etnaviv_stretch_test)

.PHONY: all clean check-env
all: $(TESTS)

# write_bmp.o lives in the (possibly read-only) libdrm tree; build into build/.
$(BUILDDIR)/write_bmp.o: $(ETNA_H)/write_bmp.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/etna2d_util.o: etna2d_util.c etna2d_util.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/etnaviv_%_test: etnaviv_%_test.c \
		$(BUILDDIR)/etna2d_util.o $(BUILDDIR)/write_bmp.o etna2d_util.h
	$(CC) $(CFLAGS) $(LDFLAGS) $< \
		$(BUILDDIR)/etna2d_util.o $(BUILDDIR)/write_bmp.o \
		$(LDLIBS) -o $@

check-env:
	@echo "CC       = $(CC)"
	@echo "SYSROOT  = $(SYSROOT)"
	@echo "LIBDRM   = $(LIBDRM)"
	@test -x "$(CC)" || { echo "ERROR: CC not found — source the SDK env or set BR"; exit 1; }
	@test -f "$(ETNA_H)/state_2d.xml.h" || { echo "ERROR: etnaviv headers not found under $(ETNA_H)"; exit 1; }
	@test -f "$(SYSROOT)/usr/lib/libdrm_etnaviv.so" || { echo "ERROR: libdrm_etnaviv not in sysroot"; exit 1; }
	@echo "environment OK"

clean:
	rm -rf $(BUILDDIR)
