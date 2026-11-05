SHELL := /bin/bash

LIB := libmario-ui.a
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)

OPTIMIZE ?= -O0
CFLAGS := -c -Iinclude -std=gnu11 -Wall -Werror \
	-ggdb $(OPTIMIZE) -fno-omit-frame-pointer -MMD -MP
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJECTS)
	ar rcsD $@ $^
	ranlib -D $@

src/%.o: src/%.c include/gui/surface.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(LIB) $(OBJECTS) $(DEPS)

-include $(DEPS)
