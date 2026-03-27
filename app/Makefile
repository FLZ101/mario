SUBDIRS	:= $(patsubst %/main.c,%, $(wildcard */main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard */*/main.c))

SHELL=/bin/bash

.PHONY: all clean

ROOT := $(shell pwd)
export ROOT

MESSAGE := $(shell $(CC) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a compiler for mario)
endif
MESSAGE := $(shell $(LD) --mario-verify 2>&1)
ifneq ($(MESSAGE),)
  $(error Not a linker for mario)
endif

all:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x -f $(ROOT)/Makefile.in; done

clean:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x -f $(ROOT)/Makefile.in; done
