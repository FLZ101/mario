SHELL := /bin/bash

ROOT := $(CURDIR)
OPTIMIZE ?= -O0
export OPTIMIZE
JOBS ?= $(shell nproc)
export JOBS

ifneq ($(filter default undefined,$(origin CXX)),)
CXX := $(abspath $(ROOT)/../../util/mario-musl-c++)
endif
CXX_PYTHON ?= $(abspath $(ROOT)/../../.venv/bin/python)

MAKEOVERRIDES := $(filter-out COMPILE_COMMANDS_JSON=%,$(MAKEOVERRIDES))
unexport COMPILE_COMMANDS_JSON

.PHONY:: all build
.PHONY:: clean clean-src

all:: build

include btop.mk
