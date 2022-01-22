SUBDIRS :=init test_exec test_signal test_fork
MAKE :=make

SHELL=/bin/bash

.PHONY: all clean

all:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) all -C $$x ; done

clean:
	@set -e ; for x in $(SUBDIRS) ; do $(MAKE) clean -C $$x ; done
