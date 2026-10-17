RM	:= rm -f

SHELL := /bin/bash
export SHELL

ROOT := $(shell pwd)
PYTHON := $(ROOT)/.venv/bin/python
VENV_STAMP := $(ROOT)/.venv/.stamp
CASES ?= tests/shell

.PHONY: all kernel rd app libc image run debug \
	quick-run quick-debug quick-run-gtk quick-debug-gtk musl venv test precommit setup \
	compiler-rt

.PHONY: clean clean-kernel clean-rd clean-app clean-libc clean-image clean-musl clean-compiler-rt

all: kernel rd

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

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

test: venv
	$(PYTHON) tests/lit_shell_test.py $(CASES)

precommit: venv
	$(PYTHON) -m pre_commit install

setup: precommit

image: kernel rd
	$(MAKE) -C qemu

run:
	$(MAKE) run -C qemu

debug:
	$(MAKE) debug -C qemu

quick-run:
	$(MAKE) quick-run -C qemu

quick-debug:
	$(MAKE) quick-debug -C qemu

quick-run-gtk:
	$(MAKE) QEMU_DISPLAY=gtk quick-run -C qemu

quick-debug-gtk:
	$(MAKE) QEMU_DISPLAY=gtk quick-debug -C qemu

clean: clean-kernel clean-rd clean-app clean-libc clean-image clean-compiler-rt
	$(RM) $(COMPILE_COMMANDS_JSON)

clean-kernel:
	$(MAKE) clean -C src

clean-rd:
	$(MAKE) clean -C rd

clean-app:
	$(MAKE) clean -C app

clean-libc:
	$(MAKE) clean -C libc

clean-musl:
	$(MAKE) clean -C musl

clean-image:
	$(MAKE) clean -C qemu

clean-compiler-rt:
	$(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK) clean

.PHONY: tmux-run tmux-debug tmux-attach tmux-kill
tmux-run tmux-debug tmux-attach tmux-kill:
	$(MAKE) $@ QEMU_DISPLAY=curses -C qemu
