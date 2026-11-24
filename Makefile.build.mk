MAKEOVERRIDES :=

RM	:= rm -f

SHELL := /bin/bash
export SHELL

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.PHONY: all kernel musl app compiler-rt analyze-kernel \
	selftest-kmalloc selftest-page selftest-stack selftest-ubsan \
	selftest-fb-write-generation selftest-fb-write-chunks \
	selftest-fb-transition-race selftest-fbcon-start-failure
.PHONY: clean-all clean-build clean-src clean-compiler-rt clean-kernel \
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
AUTO_VAR_INIT ?= $(if $(DEBUG_TRUE),1,0)
ifeq ($(DEBUG_TRUE),)
KMALLOC_DEBUG ?= 0
PAGE_ALLOC_DEBUG ?= 0
STACK_PROTECTOR ?= 0
else
KMALLOC_DEBUG ?= 1
PAGE_ALLOC_DEBUG ?= 1
STACK_PROTECTOR ?= 1
endif
UBSAN ?= 0
ANALYZE ?= 0

export STACK_PROTECTOR

BUILD_ENV_FILE := $(ROOT)/.build-env
override BUILD_IMAGE := $(if $(wildcard $(BUILD_ENV_FILE)),$(shell cat $(BUILD_ENV_FILE)))

BUILD_IMAGE_TAG := localhost/mario-build:$(subst :,-,$(BUILD_IMAGE))
APP_VENV_DIR := $(ROOT)/.venv$(if $(BUILD_IMAGE),/build-$(subst :,-,$(BUILD_IMAGE)))

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
APP_CXX	:= $(ROOT)/util/mario-musl-c++
APP_LD	:= $(ROOT)/util/mario-musl-ld

compiler-rt:
	$(PODMAN_RUN) $(MAKE) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld -f $(COMPILER_RT_MK)

kernel: compiler-rt
	$(PODMAN_RUN) $(MAKE) $(BUILD_MAKE_FLAGS) CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld \
		AUTO_VAR_INIT=$(AUTO_VAR_INIT) \
		KMALLOC_DEBUG=$(KMALLOC_DEBUG) \
		PAGE_ALLOC_DEBUG=$(PAGE_ALLOC_DEBUG) \
		STACK_PROTECTOR=$(STACK_PROTECTOR) \
		UBSAN=$(UBSAN) -C src

analyze-kernel: compiler-rt
	$(PODMAN_RUN) $(MAKE) OPTIMIZE=-O0 CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld \
		AUTO_VAR_INIT=0 KMALLOC_DEBUG=0 PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 \
		UBSAN=0 ANALYZE=1 -C src

SELF_TEST_FLAGS := OPTIMIZE=-O0 CC=$(ROOT)/util/mario-cc LD=$(ROOT)/util/mario-ld

# Self-test macros change CFLAGS but are not object dependencies, so each
# target must discard objects compiled for a different kernel configuration.
selftest-kmalloc: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=1 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 UBSAN=0 \
		KMALLOC_SELF_TEST=1 -C src

selftest-page: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=1 STACK_PROTECTOR=0 UBSAN=0 \
		PAGE_ALLOC_SELF_TEST=1 -C src

selftest-stack: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=1 UBSAN=0 \
		STACK_PROTECTOR_SELF_TEST=1 -C src

selftest-ubsan: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 UBSAN=1 \
		UBSAN_SELF_TEST=1 -C src

selftest-fb-write-generation: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 UBSAN=0 \
		FB_WRITE_GENERATION_SELF_TEST=1 -C src

selftest-fb-write-chunks: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 UBSAN=0 \
		FB_WRITE_CHUNK_SELF_TEST=1 -C src

selftest-fb-transition-race: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 UBSAN=0 \
		FB_TRANSITION_RACE_SELF_TEST=1 -C src

selftest-fbcon-start-failure: clean-kernel compiler-rt
	$(PODMAN_RUN) $(MAKE) $(SELF_TEST_FLAGS) KMALLOC_DEBUG=0 \
		PAGE_ALLOC_DEBUG=0 STACK_PROTECTOR=0 UBSAN=0 \
		FBCON_START_FAILURE_SELF_TEST=1 -C src

musl: compiler-rt
	$(PODMAN_RUN) $(MAKE) $(BUILD_MAKE_FLAGS) CC=$(ROOT)/util/mario-cc COMPILE_COMMANDS_JSON= -C musl

$(APP_VENV_DIR)/.stamp: $(ROOT)/requirements.txt $(ROOT)/Makefile.setup.mk
	$(PODMAN_RUN) $(MAKE) -f Makefile.setup.mk VENV_DIR="$(APP_VENV_DIR)" venv

app: musl $(APP_VENV_DIR)/.stamp
	$(PODMAN_RUN) env COMPILE_COMMANDS_JSON="$(COMPILE_COMMANDS_JSON)" \
		$(MAKE) $(BUILD_MAKE_FLAGS) CC=$(APP_CC) CXX="$(APP_CXX)" LD=$(APP_LD) \
		MPV_PYTHON="$(APP_VENV_DIR)/bin/python" CXX_PYTHON="$(APP_VENV_DIR)/bin/python" -C app

clean-all: clean-build clean-src

clean-build: clean-kernel clean-compiler-rt clean-musl clean-app
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
