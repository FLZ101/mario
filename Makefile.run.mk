MAKEOVERRIDES :=

SHELL := /bin/bash
export SHELL

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
QEMU_DIR := $(ROOT)/qemu

PYTHON ?= $(ROOT)/.venv/bin/python

SHELL_TEST_CASES ?= $(ROOT)/tests/shell
TTY_TEST_CASES ?= $(ROOT)/tests/tty
GUI_TEST_CASES ?= $(ROOT)/tests/gui

SMP ?= 1
export SMP

LIT_JOBS ?= 4

.PHONY: run debug quick-run quick-debug \
	run-gtk debug-gtk quick-run-gtk quick-debug-gtk \
	tmux-run tmux-debug tmux-attach tmux-kill \
	test shell-test tty-test gui-test gui-parser-test

.DEFAULT_GOAL := run

run:
	$(MAKE) -C $(QEMU_DIR) run

debug:
	$(MAKE) -C $(QEMU_DIR) debug

quick-run:
	$(MAKE) -C $(QEMU_DIR) quick-run

quick-debug:
	$(MAKE) -C $(QEMU_DIR) quick-debug

run-gtk:
	$(MAKE) QEMU_DISPLAY=gtk BOOT_DISK=hd1-gtk.img USR_DISK=hd3-gtk.img -C $(QEMU_DIR) run

debug-gtk:
	$(MAKE) QEMU_DISPLAY=gtk BOOT_DISK=hd1-gtk.img USR_DISK=hd3-gtk.img -C $(QEMU_DIR) debug

quick-run-gtk:
	$(MAKE) QEMU_DISPLAY=gtk -C $(QEMU_DIR) quick-run

quick-debug-gtk:
	$(MAKE) QEMU_DISPLAY=gtk -C $(QEMU_DIR) quick-debug

tmux-run:
	$(MAKE) QEMU_DISPLAY=curses -C $(QEMU_DIR) tmux-run

tmux-debug:
	$(MAKE) QEMU_DISPLAY=curses -C $(QEMU_DIR) tmux-debug

tmux-attach:
	$(MAKE) -C $(QEMU_DIR) tmux-attach

tmux-kill:
	$(MAKE) -C $(QEMU_DIR) tmux-kill

.PHONY: image-builder-test net-test

test: image-builder-test net-test shell-test tty-test gui-test

net-test:
	PYTHONPATH=$(ROOT)/tests $(PYTHON) $(ROOT)/tests/net/boot.py --smp $(SMP)
	PYTHONPATH=$(ROOT)/tests $(PYTHON) $(ROOT)/tests/net/ethernet.py --smp $(SMP)

image-builder-test:
	$(PYTHON) $(ROOT)/tests/test_mkdisk.py
	$(PYTHON) $(ROOT)/tests/test_pexpect_utils.py
	$(PYTHON) $(ROOT)/tests/test_gui_drawing_benchmark.py

shell-test:
	$(PYTHON) $(ROOT)/tests/lit_shell_test.py --smp $(SMP) --jobs $(LIT_JOBS) $(SHELL_TEST_CASES)

tty-test:
	$(PYTHON) $(ROOT)/tests/lit_tty_test.py --smp $(SMP) --jobs $(LIT_JOBS) $(TTY_TEST_CASES)

gui-test: gui-parser-test
	$(PYTHON) $(ROOT)/tests/lit_gui_test.py --smp $(SMP) --jobs $(LIT_JOBS) $(GUI_TEST_CASES)

gui-parser-test:
	$(PYTHON) $(ROOT)/tests/test_lit_gui_parser.py
	$(PYTHON) $(ROOT)/tests/test_lit_skip.py
