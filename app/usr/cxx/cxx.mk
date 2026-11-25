CXX_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
# The C runtime is configured with the musl C wrapper rather than host cc so
# CMake's C compiler checks use Mario's compiler-rt and musl toolchain paths.
MARIO_CC := $(abspath $(CXX_DIR)/../../../util/mario-musl-cc)
MARIO_CXX := $(abspath $(CXX_DIR)/../../../util/mario-musl-c++)
RUNTIME_CXX := $(abspath $(CXX_DIR)/../../../util/mario-c++)
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
JOBS ?= $(shell nproc)
export JOBS

.PHONY:: build-cxx-runtime clean-cxx clean-src-cxx
build:: build-cxx-runtime
build-cxx-runtime: $(CXX_TOOLCHAIN_STAMP)

$(CXX_RUNTIME_STAMP): $(CXX_DIR)/build-runtime.sh $(CXX_DIR)/llvm-sources.sha256 \
		$(MARIO_CC) $(RUNTIME_CXX) \
		$(CXX_DIR)/libcxx-gcc.patch $(CXX_DIR)/libcxx-mario.patch $(CXX_DIR)/libcxxabi-gcc.patch $(CXX_LIBC)
	CC="$(MARIO_CC)" CXX="$(RUNTIME_CXX)" PYTHON="$(CXX_PYTHON)" CXX_DIR="$(CXX_DIR)" CXX_BUILD_DIR="$(CXX_BUILD_DIR)" \
		CXX_INSTALL_DIR="$(CXX_INSTALL_DIR)" MUSL_DIR="$(abspath $(dir $(CXX_LIBC))/..)" \
		bash "$(CXX_DIR)/build-runtime.sh"
	touch "$@"

$(CXX_TOOLCHAIN_STAMP): $(CXX_RUNTIME_STAMP) $(CXX_LINK_DEPS)
	touch "$@"

clean:: clean-cxx
clean-cxx:
	$(RM) -r "$(CXX_BUILD_DIR)" "$(CXX_INSTALL_DIR)"

clean-src:: clean-src-cxx
clean-src-cxx:
	$(RM) -r "$(CXX_DIR)/downloads"
