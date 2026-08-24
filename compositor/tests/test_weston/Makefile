# Makefile — test_weston (Step 6: Weston linux-dmabuf-v1 plane-promotion gate)
#
# Bare-C cross-build.  Called by compositor/Makefile as:
#   $(MAKE) -C build/test_weston -f $(CURDIR)/tests/test_weston/Makefile \
#           SRCDIR=$(CURDIR)/tests/test_weston
#
# Prerequisites:
#   source ~/Documents/workspace/ma35d1/ma35d1_br24_qt5/output/host/environment-setup

CC      ?= aarch64-linux-gcc
PKGCONF ?= pkg-config

CFLAGS  += -std=c11 -Wall -Wextra -g -O2 \
           $(shell $(PKGCONF) --cflags wayland-client libdrm)

LIBS    := $(shell $(PKGCONF) --libs wayland-client libdrm)

SRCDIR  ?= .

SRCS := $(SRCDIR)/test_weston.c \
        $(SRCDIR)/linux-dmabuf-unstable-v1-protocol.c \
        $(SRCDIR)/xdg-shell-protocol.c

OBJS := $(notdir $(SRCS:.c=.o))

TARGET := test_weston

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Pattern rule: compile from SRCDIR
%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
