CC ?= gcc
CFLAGS ?= -O2 -pipe -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
PKG_CONFIG ?= pkg-config
PKGS = x11 xext

TARGET = build/touchscreen-rmb-winlike-c
SRC = src/main.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(shell $(PKG_CONFIG) --cflags --libs $(PKGS)) $(LDFLAGS)

clean:
	rm -rf build

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/touchscreen-rmb-winlike-c
