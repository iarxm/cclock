CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config

PKGS = x11 xfixes xrender xext xft
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

TARGETS = cclock cclockctl
SRC = src/main.c
CTL_SRC = src/cclockctl.c
CFG = src/config.h
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall

all: $(TARGETS)

cclock: $(SRC) $(CFG)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

cclockctl: $(CTL_SRC) $(CFG)
	$(CC) $(CFLAGS) -o $@ $(CTL_SRC)

install: $(TARGETS)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 cclock cclockctl "$(DESTDIR)$(BINDIR)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/cclock" "$(DESTDIR)$(BINDIR)/cclockctl"

clean:
	rm -f $(TARGETS)
