CXX_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
MARIO_CXX := $(abspath $(CXX_DIR)/../../../util/mario-musl-c++)
CXX_LIBC := $(abspath $(CXX_DIR)/../../../musl/musl-1.2.5-install/lib/libc.a)
CXX_LINK_DEPS := $(MARIO_CXX) $(CXX_LIBC) $(CXX_DIR)/../../../musl/musl.ld \
	$(CXX_DIR)/../../../compiler-rt/libcompiler_rt.a
CXX_BUILD_DIR := $(CXX_DIR)/build
CXX_INSTALL_DIR := $(CXX_DIR)/install
CXX_PYTHON ?= $(abspath $(CXX_DIR)/../../../.venv/bin/python)
MARIO_CXX_CMD = "$(CXX_PYTHON)" "$(MARIO_CXX)"
CXX_RUNTIME_STAMP := $(CXX_INSTALL_DIR)/.stamp

$(CXX_RUNTIME_STAMP): $(CXX_DIR)/build-runtime.sh $(CXX_DIR)/llvm-sources.sha256 \
		$(CXX_DIR)/libcxx-gcc.patch $(CXX_DIR)/libcxx-mario.patch $(CXX_DIR)/libcxxabi-gcc.patch $(CXX_LIBC)
	PYTHON="$(CXX_PYTHON)" CXX_DIR="$(CXX_DIR)" CXX_BUILD_DIR="$(CXX_BUILD_DIR)" \
		CXX_INSTALL_DIR="$(CXX_INSTALL_DIR)" MUSL_DIR="$(abspath $(dir $(CXX_LIBC))/..)" \
		bash "$(CXX_DIR)/build-runtime.sh"
	touch "$@"
