SUBDIRS	:= $(patsubst %/main.c,%, $(wildcard */main.c))
MAKE :=make

SHELL=/bin/bash

.PHONY: all clean

all:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x -f ../Makefile.in; done

clean:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x -f ../Makefile.in; done
