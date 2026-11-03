MAKEOVERRIDES :=

SHELL := /bin/bash
export SHELL

ROOT := $(shell pwd)

.PHONY: all clean clean-all

all: kernel app

clean: clean-build

clean-all: clean-build clean-disk-image clean-container

# ------

.PHONY: venv precommit-install precommit precommit-all

venv precommit-install precommit precommit-all:
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

.PHONY: container clean-container clean-container-env clean-containers

container: $(BUILD_ENV_CLEAN)

container:
	@if [ -n '$(BUILD_IMAGE)' ]; then \
		make -f Makefile.container.mk BUILD_IMAGE=$(BUILD_IMAGE) container; \
	fi

clean-container:
	@if [ -n '$(BUILD_IMAGE)' ]; then \
		make -f Makefile.container.mk BUILD_IMAGE=$(BUILD_IMAGE) clean-container; \
	fi

clean-container-env:
	@test -f '$(BUILD_ENV_FILE)' || { echo 'not in a container build: $(BUILD_ENV_FILE) does not exist' >&2; exit 1; }
	make -f Makefile.build.mk clean-build
	make -f Makefile.container.mk clean-container-env

clean-containers:
	make -f Makefile.container.mk clean-containers

# ------

.PHONY: kernel app analyze-kernel
.PHONY: clean-build clean-kernel clean-app clean-src

DEBUG ?= 1
OPTIMIZE ?= -O0
DEBUG_TRUE := $(filter 1 true TRUE True,$(DEBUG))
AUTO_VAR_INIT ?= $(if $(DEBUG_TRUE),1,0)
KMALLOC_DEBUG ?= $(if $(DEBUG_TRUE),1,0)
PAGE_ALLOC_DEBUG ?= $(if $(DEBUG_TRUE),1,0)
STACK_PROTECTOR ?= $(if $(DEBUG_TRUE),1,0)
UBSAN ?= 0
export STACK_PROTECTOR

kernel app:
	make -f Makefile.build.mk DEBUG=$(DEBUG) OPTIMIZE=$(OPTIMIZE) \
		AUTO_VAR_INIT=$(AUTO_VAR_INIT) \
		KMALLOC_DEBUG=$(KMALLOC_DEBUG) \
		PAGE_ALLOC_DEBUG=$(PAGE_ALLOC_DEBUG) \
		STACK_PROTECTOR=$(STACK_PROTECTOR) \
		UBSAN=$(UBSAN) $@

analyze-kernel:
	make -f Makefile.build.mk DEBUG=0 OPTIMIZE=-O0 ANALYZE=1 analyze-kernel

clean-build clean-kernel clean-app clean-src:
	make -f Makefile.build.mk $@

# ------

.PHONY: disk-image disk-image-vbe_1024x768 disk-image-vbe_1920x1080 \
	disk-image-text clean-disk-image

disk-image disk-image-vbe_1024x768 disk-image-vbe_1920x1080 disk-image-text: venv

disk-image:
	make -f Makefile.image.mk BOOT_MENU=menu.lst

disk-image-vbe_1024x768:
	make -f Makefile.image.mk BOOT_MENU=menu.lst.vbe_1024x768

disk-image-vbe_1920x1080:
	make -f Makefile.image.mk BOOT_MENU=menu.lst.vbe_1920x1080

disk-image-text:
	make -f Makefile.image.mk BOOT_MENU=menu.lst.text

clean-disk-image:
	make -f Makefile.image.mk clean

# ------

.PHONY: test test-all

SMP ?= 1
LIT_JOBS ?= 4
TEST_SMPS ?= 1 2 4 8 16

test: venv disk-image-vbe_1024x768
	make -f Makefile.run.mk SMP=$(SMP) LIT_JOBS=$(LIT_JOBS) $@

test-all:
	@set -e; for smp in $(TEST_SMPS); do \
		echo "==> make test SMP=$$smp"; \
		$(MAKE) SMP=$$smp LIT_JOBS=$(LIT_JOBS) test; \
	done

# ------

.PHONY: run debug quick-run quick-debug \
	run-gtk debug-gtk quick-run-gtk quick-debug-gtk \
	tmux-run tmux-debug tmux-attach tmux-kill

run debug quick-run quick-debug quick-run-gtk quick-debug-gtk \
	tmux-run tmux-debug tmux-attach tmux-kill: disk-image-text

run-gtk debug-gtk: disk-image

run debug quick-run quick-debug \
	run-gtk debug-gtk quick-run-gtk quick-debug-gtk \
	tmux-run tmux-debug tmux-attach tmux-kill:
	make -f Makefile.run.mk SMP=$(SMP) LIT_JOBS=$(LIT_JOBS) $@
