# Makefile for Imagnoter - native GTK4 YOLO annotator
#
#   make            build (optimized)
#   make debug      rebuild with -O0 -g3 + AddressSanitizer/UBSan
#   make run        build and launch
#   make install    install to $(PREFIX)/bin   (PREFIX defaults to /usr/local)
#   make uninstall  remove it
#   make clean      remove the binary
#   make check      print the detected GTK4 version
#
# Override anything on the command line, e.g.:
#   make CC=clang
#   make PREFIX=$HOME/.local install

CC         ?= cc
PKG_CONFIG ?= pkg-config
INSTALL    ?= install
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin

BIN := imagnoter
SRC := imagnoter.c

GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk4)
GTK_LIBS   := $(shell $(PKG_CONFIG) --libs gtk4)

# CFLAGS uses ?= so the user can replace the base flags; the rest is appended.
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra $(GTK_CFLAGS)
LDLIBS := $(GTK_LIBS) -lm

.PHONY: all debug run install uninstall clean check

all: $(BIN)

$(BIN): $(SRC)
	@$(PKG_CONFIG) --exists gtk4 || { \
		echo "error: gtk4 not found - install the GTK4 dev package"; \
		echo "       Debian/Ubuntu/Mint: sudo apt install libgtk-4-dev"; \
		echo "       Fedora:             sudo dnf install gtk4-devel"; \
		echo "       Arch:               sudo pacman -S gtk4"; \
		exit 1; }
	$(CC) $(CFLAGS) $< $(LDLIBS) -o $@

# One gcc invocation compiles and links, so -fsanitize in CFLAGS covers both.
debug:
	$(MAKE) clean
	$(MAKE) "CFLAGS=-O0 -g3 -Wall -Wextra -fsanitize=address,undefined $(GTK_CFLAGS)"

run: all
	./$(BIN)

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

check:
	@$(PKG_CONFIG) --modversion gtk4

clean:
	rm -f $(BIN)
