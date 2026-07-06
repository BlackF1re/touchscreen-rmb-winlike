CC ?= gcc
CFLAGS ?= -O2 -pipe -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
APPDIR ?= $(PREFIX)/share/applications
SYSTEMD_USER_DIR ?= $(PREFIX)/lib/systemd/user

COMMON_SRC = src/config.c
DAEMON_SRC = src/touchrmb_daemon.c $(COMMON_SRC)
SETTINGS_SRC = src/touchrmb_settings.c $(COMMON_SRC)

DAEMON_TARGET = build/touchrmb
SETTINGS_TARGET = build/touchrmb-settings

DAEMON_PKGS = x11 xext
SETTINGS_PKGS = gtk+-3.0

.PHONY: all clean install

all: $(DAEMON_TARGET) $(SETTINGS_TARGET)

$(DAEMON_TARGET): $(DAEMON_SRC) src/config.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(shell $(PKG_CONFIG) --cflags --libs $(DAEMON_PKGS)) $(LDFLAGS)

$(SETTINGS_TARGET): $(SETTINGS_SRC) src/config.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SETTINGS_SRC) $(shell $(PKG_CONFIG) --cflags --libs $(SETTINGS_PKGS)) $(LDFLAGS)

clean:
	rm -rf build

install: all
	install -Dm755 $(DAEMON_TARGET) $(DESTDIR)$(BINDIR)/touchrmb
	install -Dm755 $(SETTINGS_TARGET) $(DESTDIR)$(BINDIR)/touchrmb-settings
	install -Dm755 packaging/bin/run-touchrmb-in-lxqt-session.sh $(DESTDIR)$(BINDIR)/run-touchrmb-in-lxqt-session.sh
	install -Dm644 packaging/systemd-user/touchrmb.service $(DESTDIR)$(SYSTEMD_USER_DIR)/touchrmb.service
	install -Dm644 packaging/applications/touchrmb.desktop $(DESTDIR)$(APPDIR)/touchrmb.desktop
