MAKEOVERRIDES :=

SHELL := /bin/bash
export SHELL

ROOT := $(shell pwd)

.PHONY: all clean

all: kernel app

clean: clean-kernel clean-app

# ------

.PHONY: venv precommit precommit-all

venv precommit precommit-all:
	make -f Makefile.setup.mk $@

# ------

BUILD_ENV_FILE := $(ROOT)/.build-env
LAST_BUILD_IMAGE := $(if $(wildcard $(BUILD_ENV_FILE)),$(shell cat $(BUILD_ENV_FILE)))

BUILD_IMAGE ?= $(LAST_BUILD_IMAGE)

ifneq ($(BUILD_IMAGE),$(LAST_BUILD_IMAGE))
BUILD_ENV_CLEAN := clean-build-env
endif

.PHONY: clean-build-env

clean-build-env: clean-kernel clean-app

# ------

.PHONY: container clean-container

container: $(BUILD_ENV_CLEAN)

container clean-container:
	@if [ -n '$(BUILD_IMAGE)' ]; then \
		make -f Makefile.container.mk BUILD_IMAGE=$(BUILD_IMAGE) $@; \
	fi

# ------

.PHONY: kernel app
.PHONY: clean-kernel clean-app

DEBUG ?= 1
OPTIMIZE ?= -O0

kernel app clean-kernel clean-app:
	make -f Makefile.build.mk DEBUG=$(DEBUG) OPTIMIZE=$(OPTIMIZE) $@

# ------

.PHONY: image clean-image

BOOT_MENU ?= menu.lst

image:
	make -f Makefile.image.mk BOOT_MENU=$(BOOT_MENU)

clean-image:
	make -f Makefile.image.mk clean

# ------

.PHONY: test

SMP ?= 1
LIT_JOBS ?= 4

test: image
	make -f Makefile.run.mk SMP=$(SMP) LIT_JOBS=$(LIT_JOBS) $@
