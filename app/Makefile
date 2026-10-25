SUBDIRS	:= $(patsubst %/main.c,%, $(wildcard example/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard test/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard test/musl/*/main.c))

SHELL := /bin/bash

.PHONY: all example usr check
.PHONY: clean clean-example clean-usr

ROOT := $(shell pwd)
export ROOT
OPTIMIZE ?= -O3
export OPTIMIZE

all: example usr | check

example:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x -f $(ROOT)/Makefile.in; done

clean: clean-example

clean-example:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x -f $(ROOT)/Makefile.in; done

check:
	@$(CC) --mario-verify
	@$(LD) --mario-verify

usr:
	$(MAKE) COMPILE_COMMANDS_JSON= -C usr

clean-usr:
	$(MAKE) -C usr clean
