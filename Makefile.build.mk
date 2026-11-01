MAKEOVERRIDES :=

RM	:= rm -f

SHELL := /bin/bash
export SHELL

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.PHONY: all kernel musl app compiler-rt
.PHONY: clean-all clean-src clean-compiler-rt clean-kernel \
	clean-app clean-app-example clean-app-usr clean-app-usr-src \
	clean-musl clean-musl-src

all: kernel app

COMPILE_COMMANDS_JSON := $(ROOT)/compile_commands.json
export COMPILE_COMMANDS_JSON

OPTIMIZE ?= -O0
export OPTIMIZE

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

BUILD_ENV_FILE := $(ROOT)/.build-env
override BUILD_IMAGE := $(if $(wildcard $(BUILD_ENV_FILE)),$(shell cat $(BUILD_ENV_FILE)))

BUILD_IMAGE_TAG := localhost/mario-build:$(subst :,-,$(BUILD_IMAGE))

PODMAN ?= podman
PODMAN_RUN_FLAGS ?= --rm -v "$(ROOT):$(ROOT)" -w "$(ROOT)"
PODMAN_RUN = $(if $(BUILD_IMAGE),$(PODMAN) run $(PODMAN_RUN_FLAGS) "$(BUILD_IMAGE_TAG)")
BUILD_MAKE_FLAGS := OPTIMIZE=$(OPTIMIZE)
# Clean targets still parse submake files that verify the mario CC/LD
# wrappers, so pass them explicitly instead of relying on podman environment
# inheritance.
CLEAN_MAKE_FLAGS := CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld

CC	:= $(ROOT)/util/cc
export CC
LD	:= $(ROOT)/util/ld
export LD

COMPILER_RT_MK := $(ROOT)/compiler-rt/compiler-rt.mk

APP_CC	:= $(ROOT)/util/mario-musl-cc
APP_LD	:= $(ROOT)/util/mario-musl-ld

compiler-rt:
	$(PODMAN_RUN) $(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK)

kernel: compiler-rt
	$(PODMAN_RUN) $(MAKE) $(BUILD_MAKE_FLAGS) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld \
		KMALLOC_DEBUG=$(KMALLOC_DEBUG) \
		PAGE_ALLOC_DEBUG=$(PAGE_ALLOC_DEBUG) \
		STACK_PROTECTOR=$(STACK_PROTECTOR) -C src

musl: compiler-rt
	$(PODMAN_RUN) $(MAKE) $(BUILD_MAKE_FLAGS) CC=$(ROOT)/util/mario-cc COMPILE_COMMANDS_JSON= -C musl

app: musl
	$(PODMAN_RUN) $(MAKE) $(BUILD_MAKE_FLAGS) CC=$(APP_CC) LD=$(APP_LD) -C app

clean-all: clean-kernel clean-compiler-rt clean-musl clean-app
	$(RM) $(COMPILE_COMMANDS_JSON)

clean-app: clean-app-example clean-app-usr

clean-src: clean-app-usr-src clean-musl-src

clean-compiler-rt:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) -f $(COMPILER_RT_MK) clean

clean-kernel:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) clean -C src

clean-app-example:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) clean -C app

clean-app-usr:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) clean -C app/usr

clean-app-usr-src:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) clean-src -C app/usr

clean-musl:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) clean -C musl

clean-musl-src:
	$(PODMAN_RUN) $(MAKE) $(CLEAN_MAKE_FLAGS) clean-src -C musl
