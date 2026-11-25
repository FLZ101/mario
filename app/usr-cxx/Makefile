SHELL := /bin/bash

ROOT := $(CURDIR)
OPTIMIZE ?= -O0
export OPTIMIZE
JOBS ?= $(shell nproc)
export JOBS

ifneq ($(filter default undefined,$(origin CXX)),)
CXX := $(abspath $(ROOT)/../../util/mario-musl-c++)
endif
ifneq ($(filter default undefined,$(origin CC)),)
CC := $(abspath $(ROOT)/../../util/mario-musl-cc)
endif
CXX_PYTHON ?= $(abspath $(ROOT)/../../.venv/bin/python)

FLTK_COMPILE_COMMANDS_JSON := $(COMPILE_COMMANDS_JSON)
MAKEOVERRIDES := $(filter-out COMPILE_COMMANDS_JSON=%,$(MAKEOVERRIDES))
unexport COMPILE_COMMANDS_JSON

.PHONY:: all build
.PHONY:: clean clean-src

all:: build

include btop.mk
include fltk.mk

.PHONY:: build-filemanager clean-filemanager
build:: build-filemanager
build-filemanager: build-fltk
	$(MAKE) -C filemanager -f ../../Makefile.cxx.in CC="$(CC)" CXX="$(CXX)" \
		COMPILE_COMMANDS_JSON="$(FLTK_COMPILE_COMMANDS_JSON)"

clean:: clean-filemanager
clean-filemanager:
	$(MAKE) -C filemanager -f ../../Makefile.cxx.in clean

.PHONY:: build-system-monitor clean-system-monitor
build:: build-system-monitor
build-system-monitor: build-fltk
	$(MAKE) -C system-monitor -f ../../Makefile.cxx.in CC="$(CC)" CXX="$(CXX)" \
		COMPILE_COMMANDS_JSON="$(FLTK_COMPILE_COMMANDS_JSON)"

clean:: clean-system-monitor
clean-system-monitor:
	$(MAKE) -C system-monitor -f ../../Makefile.cxx.in clean
