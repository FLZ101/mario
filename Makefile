MAKEOVERRIDES :=

RM	:= rm -f

SHELL := /bin/bash
export SHELL

ROOT := $(shell pwd)

# Remember the last build image.  Empty means host; a set image means podman.
# Plain `make test` reads this file so it continues with the same build
# environment instead of falling back to a host rebuild after a container
# build.
BUILD_ENV_FILE := $(ROOT)/.build-env
-include $(BUILD_ENV_FILE)

PYTHON := $(ROOT)/.venv/bin/python
VENV_STAMP := $(ROOT)/.venv/.stamp
SHELL_TEST_CASES ?= tests/shell
TTY_TEST_CASES ?= tests/tty
GUI_TEST_CASES ?= tests/gui

.PHONY: all kernel rd app image run debug \
	quick-run quick-debug run-gtk debug-gtk quick-run-gtk quick-debug-gtk \
	test shell-test tty-test gui-test \
	musl venv precommit-install precommit precommit-all setup \
	compiler-rt container-image container-shell container-clean FORCE

.PHONY: clean clean-kernel clean-rd clean-app-example clean-app-usr clean-image \
	clean-musl clean-compiler-rt

all: kernel app

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

# OPTIMIZE controls code generation only; it is not a runtime-check switch.
OPTIMIZE ?= -O0
export OPTIMIZE

# DEBUG is a preset that enables all runtime checks.  Specific check
# variables use ?=, so a command-line or environment value overrides DEBUG.
DEBUG ?= 1
unexport DEBUG

DEBUG_TRUE := $(filter 1 true TRUE True,$(DEBUG))
ifeq ($(DEBUG_TRUE),)
KMALLOC_DEBUG ?= 0
PAGE_ALLOC_DEBUG ?= 0
STACK_PROTECTOR ?= 0
else
KMALLOC_DEBUG ?= 1
PAGE_ALLOC_DEBUG ?= 1
STACK_PROTECTOR ?= 1
endif

# Number of QEMU CPUs used by test and quick-run targets.  Keep the default
# at one; SMP test cases opt in with REQUIRES-SMP and are skipped otherwise.
SMP ?= 1
export SMP

BOOT_ENTRY ?= x
export BOOT_ENTRY

# Number of lit test cases run concurrently by shell-test and tty-test.
# Keep this moderate: too many concurrent QEMU instances contend for host
# disks, which can push ext2/vfat mount, read, write, and stress tests past
# their timeout. TTY tests use independent QEMU instances and host tmux
# servers, so excessive parallelism can still delay terminal redraws.
LIT_JOBS ?= 4

# Empty means build on the host; setting BUILD_IMAGE selects podman.
BUILD_IMAGE ?=
BUILD_ENV = $(if $(BUILD_IMAGE),podman,host)
# Recursive (not ?=) so a command-line BUILD_IMAGE override recomputes the tag.
BUILD_IMAGE_TAG = localhost/mario-build:$(subst :,-,$(BUILD_IMAGE))
PODMAN ?= podman
BUILD_CONTAINERFILE := .github/ci/Containerfile.$(subst :,-,$(BUILD_IMAGE))
BUILD_IMAGE_STAMP := .build-image-$(subst /,-,$(subst :,-,$(BUILD_IMAGE_TAG)))
PODMAN_RUN_FLAGS ?= --rm -v "$(ROOT):$(ROOT)" -w "$(ROOT)"

PODMAN_MAKE = $(PODMAN) run $(PODMAN_RUN_FLAGS) \
	"$(BUILD_IMAGE_TAG)" make BUILD_ENV=host

BUILD_MAKE_FLAGS := DEBUG=$(DEBUG) \
	OPTIMIZE=$(OPTIMIZE)

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

COMPILER_RT_MK := $(ROOT)/compiler-rt/compiler-rt.mk

APP_CC	:= $(ROOT)/util/mario-musl-cc
APP_LD	:= $(ROOT)/util/mario-musl-ld

# Update the persisted environment marker.  When it changes, clean the
# affected host/container build outputs first so artifacts from the old
# environment are never reused for the new one.  Skip that clean under
# `make -n`, where MAKEFLAGS contains `n`.
$(BUILD_ENV_FILE): FORCE
	@printf 'BUILD_IMAGE := %s\n' '$(BUILD_IMAGE)' > $@.tmp
	@if [ -f $@ ]; then \
		if cmp -s $@.tmp $@ 2>/dev/null; then \
			$(RM) $@.tmp; \
		else \
			mv $@.tmp $@; \
			if [ -z "$(filter n,$(MAKEFLAGS))" ]; then \
				$(MAKE) clean-kernel clean-compiler-rt clean-musl \
					clean-app-example clean-app-usr; \
			fi; \
		fi; \
	else \
		mv $@.tmp $@; \
	fi

# Every build target records the environment before invoking its sub-make.
compiler-rt kernel musl app: $(BUILD_ENV_FILE)

$(BUILD_IMAGE_STAMP): $(BUILD_CONTAINERFILE)
	@if ! $(PODMAN) image exists "$(BUILD_IMAGE_TAG)" >/dev/null 2>&1; then \
		$(PODMAN) build -t "$(BUILD_IMAGE_TAG)" -f "$(BUILD_CONTAINERFILE)" . ; \
	fi
	@touch $@

ifeq ($(BUILD_ENV),podman)
compiler-rt kernel musl app: $(BUILD_IMAGE_STAMP)
	$(PODMAN_MAKE) $(BUILD_MAKE_FLAGS) $@
else
compiler-rt:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK)

kernel: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld \
		KMALLOC_DEBUG=$(KMALLOC_DEBUG) \
		PAGE_ALLOC_DEBUG=$(PAGE_ALLOC_DEBUG) \
		STACK_PROTECTOR=$(STACK_PROTECTOR) -C src

musl: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc COMPILE_COMMANDS_JSON= -C musl

app: musl
	$(MAKE) CC=$(APP_CC) LD=$(APP_LD) -C app
endif

container-image:
	$(PODMAN) build -t "$(BUILD_IMAGE_TAG)" -f "$(BUILD_CONTAINERFILE)" .
	@touch $(BUILD_IMAGE_STAMP)

container-shell: $(BUILD_IMAGE_STAMP)
	$(PODMAN) run -it $(PODMAN_RUN_FLAGS) "$(BUILD_IMAGE_TAG)" /bin/bash

container-clean:
	-$(PODMAN) image rm "$(BUILD_IMAGE_TAG)"
	$(RM) $(BUILD_IMAGE_STAMP)

$(VENV_STAMP): requirements.txt
	python3 -m venv --system-site-packages .venv
	$(PYTHON) -m pip install -r requirements.txt
	@touch $@

venv: $(VENV_STAMP)

shell-test: venv kernel rd image
	$(PYTHON) tests/lit_shell_test.py --smp $(SMP) --jobs $(LIT_JOBS) $(SHELL_TEST_CASES)

tty-test: venv kernel rd image
	$(PYTHON) tests/lit_tty_test.py --smp $(SMP) --jobs $(LIT_JOBS) $(TTY_TEST_CASES)

gui-test: venv kernel rd image
	$(PYTHON) tests/lit_gui_test.py --smp $(SMP) --jobs $(LIT_JOBS) $(GUI_TEST_CASES)

test: shell-test tty-test gui-test

precommit-install: venv
	$(PYTHON) -m pre_commit install

precommit: setup
	$(PYTHON) -m pre_commit run

precommit-all: setup
	$(PYTHON) -m pre_commit run --all-files

setup: precommit-install

rd: app
	$(MAKE) -C rd

image: kernel rd
	$(MAKE) -C qemu

run:
	$(MAKE) BOOT_ENTRY=2 image
	$(MAKE) run -C qemu

debug:
	$(MAKE) BOOT_ENTRY=2 image
	$(MAKE) debug -C qemu

quick-run: image
	$(MAKE) quick-run -C qemu

quick-debug: image
	$(MAKE) quick-debug -C qemu

run-gtk: image
	$(MAKE) QEMU_DISPLAY=gtk run -C qemu

debug-gtk: image
	$(MAKE) QEMU_DISPLAY=gtk debug -C qemu

quick-run-gtk: image
	$(MAKE) QEMU_DISPLAY=gtk quick-run -C qemu

quick-debug-gtk: image
	$(MAKE) QEMU_DISPLAY=gtk quick-debug -C qemu

clean-all: clean clean-musl clean-app-usr

clean: clean-kernel clean-rd clean-app-example clean-image clean-compiler-rt
	$(RM) $(COMPILE_COMMANDS_JSON) $(BUILD_ENV_FILE)

clean-kernel:
	$(MAKE) clean -C src

clean-rd:
	$(MAKE) clean -C rd

clean-app-example:
	$(MAKE) clean -C app

clean-app-usr:
	$(MAKE) clean-usr -C app

clean-musl:
	$(MAKE) clean -C musl

clean-image:
	$(MAKE) clean -C qemu

clean-compiler-rt:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK) clean

.PHONY: tmux-run tmux-debug tmux-attach tmux-kill
tmux-run tmux-debug tmux-attach tmux-kill: image
	$(MAKE) $@ QEMU_DISPLAY=curses -C qemu
