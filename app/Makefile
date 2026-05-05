SUBDIRS	:= $(patsubst %/main.c,%, $(wildcard example/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard test/*/main.c))
ifeq ($(_LIBC),musl)
  SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard test/musl/*/main.c))
endif

SHELL=/bin/bash

.PHONY: all example usr check
.PHONY: clean clean-example clean-usr

ROOT := $(shell pwd)
export ROOT

all: example usr | check

example:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x -f $(ROOT)/Makefile.in; done

clean: clean-example

clean-example:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x -f $(ROOT)/Makefile.in; done

check:
	@$(CC) --mario-verify
	@$(LD) --mario-verify

ifeq ($(_LIBC),musl)
usr:
	$(MAKE) COMPILE_COMMANDS_JSON= -C usr
else
usr:
endif

clean-usr:
	$(MAKE) -C usr clean
