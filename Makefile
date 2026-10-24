RM	:= rm -f

SHELL := /bin/bash
export SHELL

ROOT := $(shell pwd)
PYTHON := $(ROOT)/.venv/bin/python
VENV_STAMP := $(ROOT)/.venv/.stamp
SHELL_TEST_CASES ?= tests/shell
VGA_TEST_CASES ?= tests/vga

.PHONY: all kernel rd app libc image run debug \
	quick-run quick-debug quick-run-gtk quick-debug-gtk \
	test shell-test vga-test \
	musl venv precommit setup \
	compiler-rt

.PHONY: clean clean-kernel clean-rd clean-app-example clean-app-usr clean-libc clean-image \
	clean-musl clean-compiler-rt

all: kernel rd

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

# DEBUG=true (or DEBUG=1) builds the kernel, libc and applications with -O0.
# The default release build uses -O3.
DEBUG ?= false
DEBUG_TRUE := $(filter true True TRUE 1,$(DEBUG))
ifeq ($(DEBUG_TRUE),)
OPTIMIZE := -O3
else
OPTIMIZE := -O0
endif
export OPTIMIZE

# Number of QEMU CPUs used by test and quick-run targets.  Keep the default
# at one; SMP test cases opt in with REQUIRES-SMP and are skipped otherwise.
SMP ?= 1
export SMP

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

_LIBC	:= musl
# _LIBC	:= libc
export _LIBC

COMPILER_RT_MK := $(ROOT)/compiler-rt/compiler-rt.mk

ifeq ($(_LIBC), libc)
  APP_CC	:= $(ROOT)/util/mario-libc-cc
  APP_LD	:= $(ROOT)/util/mario-libc-ld
else
  APP_CC	:= $(ROOT)/util/mario-musl-cc
  APP_LD	:= $(ROOT)/util/mario-musl-ld
endif

kernel: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -C src

rd: app
	$(MAKE) -C rd

app: $(_LIBC)
	$(MAKE) CC=$(APP_CC) LD=$(APP_LD) -C app

compiler-rt:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK)

libc: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc -C libc

musl: compiler-rt
	$(MAKE) CC=$(ROOT)/util/mario-cc COMPILE_COMMANDS_JSON= -C musl

$(VENV_STAMP): requirements.txt
	python3 -m venv --system-site-packages .venv
	$(PYTHON) -m pip install -r requirements.txt
	@touch $@

venv: $(VENV_STAMP)

shell-test: venv kernel rd image
	$(PYTHON) tests/lit_shell_test.py --smp $(SMP) $(SHELL_TEST_CASES)

vga-test: venv kernel rd image
	$(PYTHON) tests/lit_vga_test.py --smp $(SMP) $(VGA_TEST_CASES)

test: shell-test vga-test

precommit: venv
	$(PYTHON) -m pre_commit install

setup: precommit

image: kernel rd
	$(MAKE) -C qemu

run: image
	$(MAKE) run -C qemu

debug: image
	$(MAKE) debug -C qemu

quick-run: image
	$(MAKE) quick-run -C qemu

quick-debug: image
	$(MAKE) quick-debug -C qemu

quick-run-gtk: image
	$(MAKE) QEMU_DISPLAY=gtk quick-run -C qemu

quick-debug-gtk: image
	$(MAKE) QEMU_DISPLAY=gtk quick-debug -C qemu

clean-all: clean clean-musl clean-app-usr

clean: clean-kernel clean-rd clean-app-example clean-libc clean-image clean-compiler-rt
	$(RM) $(COMPILE_COMMANDS_JSON)

clean-kernel:
	$(MAKE) clean -C src

clean-rd:
	$(MAKE) clean -C rd

clean-app-example:
	$(MAKE) clean -C app

clean-app-usr:
	$(MAKE) clean-usr -C app

clean-libc:
	$(MAKE) clean -C libc

clean-musl:
	$(MAKE) clean -C musl

clean-image:
	$(MAKE) clean -C qemu

clean-compiler-rt:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK) clean

.PHONY: tmux-run tmux-debug tmux-attach tmux-kill
tmux-run tmux-debug tmux-attach tmux-kill: image
	$(MAKE) $@ QEMU_DISPLAY=curses -C qemu
