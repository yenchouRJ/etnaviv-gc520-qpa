# Makefile — Phase 3 KMS scanout + benchmark (cross-compiled for MA35D1).
#
# Builds the display helper (kms_display) and four programs:
#   kms_test      Milestone A — on-panel test pattern (CPU-drawn, no GPU)
#   kms_gpu_test  Milestone B — GPU renders into PRIME-imported scanout
#   bench2d       Milestone C+D — double-buffered GPU-vs-CPU FPS benchmark
#   bench_stretch StretchBlt (scaler) GPU-vs-CPU throughput benchmark
#
# Depends on libetna2d (../libetna2d): build that first, or `make` here
# which recurses into it automatically.
#
# Build:
#   A) source the Buildroot SDK env, then: make
#   B) make BR=/home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
#
# Artifacts are placed under build/.
# Deploy all binaries to the target. Run from a text VT:
#   ./kms_test /dev/dri/card0 5
#   ./kms_gpu_test /dev/dri/card0 /dev/dri/renderD128 5
#   ./bench2d --gpu -n 64 -f 300
#   ./bench2d --cpu -n 64 -f 300
#   ./bench_stretch --gpu --sweep
#   ./bench_stretch --cpu --sweep

# ---- Buildroot locations -------------------------------------------------
BR      ?= /home/joeylu/Documents/workspace/ma35d1/ma35d1_br24_qt5
HOST    ?= $(BR)/output/host
SYSROOT ?= $(HOST)/aarch64-nuvoton-linux-gnu/sysroot
CROSS   ?= $(HOST)/bin/aarch64-nuvoton-linux-gnu-

ifeq ($(origin CC),default)
CC := $(CROSS)gcc
endif

LIBDRM   ?= $(BR)/output/build/libdrm-2.4.120
ETNA_H   := $(LIBDRM)/tests/etnaviv
ETNA2D   := ../libetna2d
ETNA2D_A := $(ETNA2D)/build/libetna2d.a

# ---- Output directory ----------------------------------------------------
BUILDDIR := build
$(shell mkdir -p $(BUILDDIR))

# ---- Flags ---------------------------------------------------------------
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=gnu11
CFLAGS  += --sysroot=$(SYSROOT)
CFLAGS  += -I$(SYSROOT)/usr/include          # xf86drmMode.h, drm_fourcc.h
CFLAGS  += -I$(SYSROOT)/usr/include/libdrm   # drm.h, drm_mode.h
CFLAGS  += -I$(ETNA_H)                       # etnaviv register headers
CFLAGS  += -I$(LIBDRM)/etnaviv
CFLAGS  += -I$(ETNA2D)                       # etna2d.h
CFLAGS  += -I.

LDFLAGS ?=
LDFLAGS += --sysroot=$(SYSROOT)
LDLIBS  := -ldrm_etnaviv -ldrm

PROGS := $(addprefix $(BUILDDIR)/, kms_test kms_gpu_test bench2d bench_stretch)

.PHONY: all clean libetna2d check-env
all: $(PROGS)

# Ensure the sibling static lib is up to date.
$(ETNA2D_A): libetna2d
libetna2d:
	$(MAKE) -C $(ETNA2D) $(ETNA2D)/build/libetna2d.a

KMS_OBJ := $(BUILDDIR)/kms_display.o

$(KMS_OBJ): kms_display.c kms_display.h
	$(CC) $(CFLAGS) -c $< -o $@

# kms_test: display path only, no GPU dependency.
$(BUILDDIR)/kms_test: kms_test.c $(KMS_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -ldrm -o $@

$(BUILDDIR)/kms_gpu_test: kms_gpu_test.c $(KMS_OBJ) $(ETNA2D_A)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILDDIR)/bench2d: bench2d.c $(KMS_OBJ) $(ETNA2D_A)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILDDIR)/bench_stretch: bench_stretch.c $(KMS_OBJ) $(ETNA2D_A)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

check-env:
	@echo "CC      = $(CC)"
	@echo "SYSROOT = $(SYSROOT)"
	@test -x "$(CC)" || { echo "ERROR: CC not found — source SDK env or set BR"; exit 1; }
	@test -f "$(SYSROOT)/usr/include/xf86drmMode.h" || { echo "ERROR: xf86drmMode.h missing"; exit 1; }
	@test -f "$(SYSROOT)/usr/include/libdrm/drm_fourcc.h" || { echo "ERROR: drm_fourcc.h missing"; exit 1; }
	@echo "environment OK"

clean:
	rm -rf $(BUILDDIR)
