SHELL := /bin/bash

LIB := libmario-ui.a
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)
APP_ROOT := $(abspath ../..)
FREETYPE_INSTALL_DIR := $(APP_ROOT)/usr/freetype-2.13.3-install
LIBPNG_INSTALL_DIR := $(APP_ROOT)/usr/libpng-1.6.59-install
HEADERS := $(wildcard include/gui/*.h)

OPTIMIZE ?= -O0
CFLAGS := -c -Iinclude -I$(APP_ROOT)/include -I$(FREETYPE_INSTALL_DIR)/include \
	-I$(LIBPNG_INSTALL_DIR)/include -std=gnu11 -Wall -Werror \
	-ggdb $(OPTIMIZE) -fno-omit-frame-pointer -MMD -MP
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJECTS)
	ar rcsD $@ $^
	ranlib -D $@

src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(LIB) $(OBJECTS) $(DEPS)

-include $(DEPS)
