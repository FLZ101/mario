MAKEOVERRIDES :=

SHELL := /bin/bash
export SHELL

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
RD_DIR := $(ROOT)/rd
QEMU_DIR := $(ROOT)/qemu

BOOT_MENU ?= menu.lst

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

.PHONY: all clean rd image clean-rd clean-image

all: image

clean: clean-image clean-rd

rd:
	$(MAKE) -C $(RD_DIR)

image: rd
	$(MAKE) BOOT_MENU=$(BOOT_MENU) -C $(QEMU_DIR) image

clean-rd:
	$(MAKE) clean -C $(RD_DIR)

clean-image:
	$(MAKE) clean -C $(QEMU_DIR)
