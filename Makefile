RM	:= rm -f

SHELL := /bin/bash
export SHELL

ROOT := $(shell pwd)
PYTHON := $(ROOT)/.venv/bin/python
VENV_STAMP := $(ROOT)/.venv/.stamp
SHELL_TEST_CASES ?= tests/shell
TTY_TEST_CASES ?= tests/tty
GUI_TEST_CASES ?= tests/gui

.PHONY: all kernel rd app image run debug \
	quick-run quick-debug run-gtk debug-gtk quick-run-gtk quick-debug-gtk \
	test shell-test tty-test gui-test \
	musl venv precommit-install precommit precommit-all setup \
	compiler-rt

.PHONY: clean clean-kernel clean-rd clean-app-example clean-app-usr clean-image \
	clean-musl clean-compiler-rt

all: kernel rd

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

# OPTIMIZE controls code generation only; it is not a runtime-check switch.
OPTIMIZE ?= -O0
export OPTIMIZE

# DEBUG is a preset that enables all runtime checks.  Specific check
# variables use ?=, so a command-line or environment value overrides DEBUG.
DEBUG ?= 1
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
JOBS ?= 4

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

COMPILER_RT_MK := $(ROOT)/compiler-rt/compiler-rt.mk

APP_CC	:= $(ROOT)/util/mario-musl-cc
APP_LD	:= $(ROOT)/util/mario-musl-ld

kernel: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld \
		KMALLOC_DEBUG=$(KMALLOC_DEBUG) \
		PAGE_ALLOC_DEBUG=$(PAGE_ALLOC_DEBUG) \
		STACK_PROTECTOR=$(STACK_PROTECTOR) -C src

rd: app
	$(MAKE) -C rd

app: musl
	$(MAKE) CC=$(APP_CC) LD=$(APP_LD) -C app

compiler-rt:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK)

musl: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc COMPILE_COMMANDS_JSON= -C musl

$(VENV_STAMP): requirements.txt
	python3 -m venv --system-site-packages .venv
	$(PYTHON) -m pip install -r requirements.txt
	@touch $@

venv: $(VENV_STAMP)

shell-test: venv kernel rd image
	$(PYTHON) tests/lit_shell_test.py --smp $(SMP) --jobs $(JOBS) $(SHELL_TEST_CASES)

tty-test: venv kernel rd image
	$(PYTHON) tests/lit_tty_test.py --smp $(SMP) --jobs $(JOBS) $(TTY_TEST_CASES)

gui-test: venv kernel rd image
	$(PYTHON) tests/lit_gui_test.py --smp $(SMP) --jobs $(JOBS) $(GUI_TEST_CASES)

test: shell-test tty-test gui-test

precommit-install: venv
	$(PYTHON) -m pre_commit install

precommit: setup
	$(PYTHON) -m pre_commit run

precommit-all: setup
	$(PYTHON) -m pre_commit run --all-files

setup: precommit-install

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
	$(RM) $(COMPILE_COMMANDS_JSON)

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
