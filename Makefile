# deskctl — root-free desktop control for Linux agents
#
# Build the Wayland input helper and stage the CLI.
#
#   make          build build/wayinput
#   make test     run the smoke test (needs a Hyprland/wlroots session)
#   make install  install into $(PREFIX)/bin and $(PREFIX)/lib/deskctl

PREFIX ?= /usr/local
CC     ?= cc
PKGS   ?= wayland-client xkbcommon

CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += $(shell pkg-config --cflags $(PKGS)) -Ibuild
LDLIBS  += $(shell pkg-config --libs $(PKGS))

PROTO_XML   := protocol/virtual-keyboard-unstable-v1.xml protocol/wlr-virtual-pointer-unstable-v1.xml
PROTO_HDRS  := $(patsubst protocol/%.xml,build/%-client-protocol.h,$(PROTO_XML))
PROTO_SRCS  := $(patsubst protocol/%.xml,build/%-protocol.c,$(PROTO_XML))

.PHONY: all clean test install uninstall

all: build/wayinput

build:
	@mkdir -p build

build/%-client-protocol.h: protocol/%.xml | build
	wayland-scanner client-header $< $@

build/%-protocol.c: protocol/%.xml | build
	wayland-scanner private-code $< $@

build/wayinput: src/wayinput.c $(PROTO_HDRS) $(PROTO_SRCS) | build
	$(CC) $(CFLAGS) -o $@ src/wayinput.c $(PROTO_SRCS) $(LDLIBS)

test: all
	./tests/smoke.sh

install: all
	install -Dm755 build/wayinput $(DESTDIR)$(PREFIX)/lib/deskctl/wayinput
	install -Dm755 deskctl $(DESTDIR)$(PREFIX)/bin/deskctl
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/deskctl/README.md
	@sed -i 's|^DESKCTL_LIB = .*|DESKCTL_LIB = "$(PREFIX)/lib/deskctl"|' $(DESTDIR)$(PREFIX)/bin/deskctl

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/deskctl
	rm -rf $(DESTDIR)$(PREFIX)/lib/deskctl
	rm -f $(DESTDIR)$(PREFIX)/share/doc/deskctl/README.md

clean:
	rm -rf build
