CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS   = -lm
PREFIX  ?= $(HOME)/.local
STRIP   ?= strip

ifeq ($(OS),Windows_NT)
  EXE = .exe
endif
TARGET = saturn$(EXE)

all: $(TARGET)

$(TARGET): saturn.c
	$(CC) $(CFLAGS) saturn.c -o $(TARGET) $(LDFLAGS) $(LDLIBS)
	$(STRIP) $(TARGET)

install: $(TARGET)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -f saturn saturn.exe

.PHONY: all install uninstall clean
