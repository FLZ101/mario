SUBDIRS	:= $(patsubst %/main.c,%, $(wildcard */main.c))
SUBDIRS	+= $(patsubst %/main.c,%, $(wildcard */*/main.c))
MAKE := make

SHELL=/bin/bash

.PHONY: all clean

CWD	:= $(shell pwd)

LIBC_DIR	:= $(CWD)/../libc/lib
export LIBC_DIR

all:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x -f $(CWD)/Makefile.in; done

clean:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x -f $(CWD)/Makefile.in; done
