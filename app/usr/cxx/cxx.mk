CXX_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MARIO_CXX := $(abspath $(CXX_DIR)/../../../util/mario-musl-c++)
ifneq ($(filter default undefined,$(origin CXX)),)
CXX := $(MARIO_CXX)
endif
CXX_LIBC := $(abspath $(CXX_DIR)/../../../musl/musl-1.2.5-install/lib/libc.a)
CXX_LINK_DEPS := $(MARIO_CXX) $(CXX_LIBC) $(CXX_DIR)/../../../musl/musl.ld \
	$(CXX_DIR)/../../../compiler-rt/libcompiler_rt.a
CXX_BUILD_DIR := $(CXX_DIR)/build
CXX_INSTALL_DIR := $(CXX_DIR)/install
CXX_PYTHON ?= $(abspath $(CXX_DIR)/../../../.venv/bin/python)
CXX_RUNTIME_STAMP := $(CXX_INSTALL_DIR)/.stamp
CXX_TOOLCHAIN_STAMP := $(CXX_BUILD_DIR)/.toolchain-stamp
CXX_PROBES := $(addprefix $(CXX_BUILD_DIR)/tests/,cxx-startup.exe cxx-stdlib.exe cxx-exceptions.exe)
JOBS ?= $(shell nproc)
export JOBS

.PHONY:: build-cxx-runtime build-cxx-probes clean-cxx clean-src-cxx
build:: build-cxx-runtime build-cxx-probes
build-cxx-runtime: $(CXX_TOOLCHAIN_STAMP)
build-cxx-probes: $(CXX_PROBES)
$(CXX_PROBES): export PATH := $(dir $(CXX_PYTHON)):$(PATH)

$(CXX_RUNTIME_STAMP): $(CXX_DIR)/build-runtime.sh $(CXX_DIR)/llvm-sources.sha256 \
		$(CXX_DIR)/libcxx-gcc.patch $(CXX_DIR)/libcxx-mario.patch $(CXX_DIR)/libcxxabi-gcc.patch $(CXX_LIBC)
	PYTHON="$(CXX_PYTHON)" CXX_DIR="$(CXX_DIR)" CXX_BUILD_DIR="$(CXX_BUILD_DIR)" \
		CXX_INSTALL_DIR="$(CXX_INSTALL_DIR)" MUSL_DIR="$(abspath $(dir $(CXX_LIBC))/..)" \
		bash "$(CXX_DIR)/build-runtime.sh"
	touch "$@"

$(CXX_TOOLCHAIN_STAMP): $(CXX_RUNTIME_STAMP) $(CXX_LINK_DEPS)
	touch "$@"

$(CXX_BUILD_DIR)/tests/cxx-startup.exe: $(CXX_DIR)/startup.cpp $(CXX_LINK_DEPS) $(CXX_DIR)/cxx.mk
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && $(CXX) -std=c++17 -Wall -Wextra $(OPTIMIZE) \
		--mario-no-cxx-runtime -fno-exceptions -fno-rtti -fno-use-cxa-atexit "$<" -o "$@"

$(CXX_BUILD_DIR)/tests/cxx-stdlib.exe: $(CXX_DIR)/stdlib.cpp $(CXX_LINK_DEPS) $(CXX_RUNTIME_STAMP)
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && $(CXX) -std=c++17 -Wall -Wextra $(OPTIMIZE) "$<" -o "$@"

CXX_EXCEPTION_SOURCES := $(addprefix $(CXX_DIR)/,exceptions.cpp exceptions-throw.cpp)
$(CXX_BUILD_DIR)/tests/cxx-exceptions.exe: $(CXX_EXCEPTION_SOURCES) $(CXX_LINK_DEPS) $(CXX_RUNTIME_STAMP)
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && $(CXX) -std=c++17 -Wall -Wextra $(OPTIMIZE) $(CXX_EXCEPTION_SOURCES) -o "$@"

clean:: clean-cxx
clean-cxx:
	$(RM) -r "$(CXX_BUILD_DIR)" "$(CXX_INSTALL_DIR)"

clean-src:: clean-src-cxx
clean-src-cxx:
	$(RM) -r "$(CXX_DIR)/downloads"
