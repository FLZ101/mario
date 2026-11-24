SUBDIRS	:= $(patsubst %/main.c,%, $(wildcard example/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard system/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard test/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard test/musl/*/main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard gui/desktop/*/main.c))
CXX_TESTS := $(patsubst %/main.cpp,%,$(wildcard test-cxx/*/main.cpp))

SHELL := /bin/bash

.PHONY: all example cxx gui usr check
.PHONY: clean clean-example clean-cxx clean-gui clean-usr

ROOT := $(shell pwd)
export ROOT
OPTIMIZE ?= -O0
export OPTIMIZE

unexport SMP BOOT_ENTRY

all: example cxx | check

example: usr gui
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x -f $(ROOT)/Makefile.in; done

cxx: usr gui
	$(MAKE) -C usr-cxx build
	@set -e; for test in $(CXX_TESTS); do $(MAKE) all -C "$$test" -f "$(ROOT)/Makefile.cxx.in"; done

clean: clean-example clean-cxx clean-gui

clean-example:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x -f $(ROOT)/Makefile.in; done

clean-cxx:
	$(MAKE) -C usr-cxx clean
	@set -e; for test in $(CXX_TESTS); do $(MAKE) clean -C "$$test" -f "$(ROOT)/Makefile.cxx.in"; done

gui:
	$(MAKE) -C gui/libmario-ui

clean-gui:
	$(MAKE) -C gui/libmario-ui clean

check:
	@$(CC) --mario-verify
	@$(LD) --mario-verify

usr:
	$(MAKE) -C usr

clean-usr:
	$(MAKE) -C usr clean
