CXX_DIR := $(ROOT)/cxx
MARIO_CXX := $(abspath $(ROOT)/../../util/mario-musl-c++)
CXX_LIBC := $(abspath $(ROOT)/../../musl/musl-1.2.5-install/lib/libc.a)
CXX_LINK_DEPS := $(MARIO_CXX) $(CXX_LIBC) $(ROOT)/../../musl/musl.ld \
	$(ROOT)/../../compiler-rt/libcompiler_rt.a
CXX_BUILD_DIR := $(CXX_DIR)/build
CXX_INSTALL_DIR := $(CXX_DIR)/install
CXX_PYTHON ?= $(abspath $(ROOT)/../../.venv/bin/python)
CXX_RUNTIME_STAMP := $(CXX_INSTALL_DIR)/.stamp

.PHONY:: build-cxx-runtime build-cxx-probes clean-cxx clean-src-cxx
build:: build-cxx-probes
build-cxx-runtime: $(CXX_RUNTIME_STAMP)
build-cxx-probes: $(CXX_BUILD_DIR)/tests/cxx-startup.exe $(CXX_BUILD_DIR)/tests/cxx-stdlib.exe

$(CXX_RUNTIME_STAMP): $(CXX_DIR)/build-runtime.sh $(CXX_DIR)/llvm-sources.sha256 \
		$(CXX_DIR)/libcxx-gcc.patch $(CXX_DIR)/libcxx-mario.patch $(CXX_LIBC)
	PYTHON="$(CXX_PYTHON)" CXX_DIR="$(CXX_DIR)" CXX_BUILD_DIR="$(CXX_BUILD_DIR)" \
		CXX_INSTALL_DIR="$(CXX_INSTALL_DIR)" MUSL_DIR="$(abspath $(dir $(CXX_LIBC))/..)" \
		bash "$(CXX_DIR)/build-runtime.sh"
	touch "$@"

$(CXX_BUILD_DIR)/tests/cxx-startup.exe: $(CXX_DIR)/startup.cpp $(CXX_LINK_DEPS) $(CXX_DIR)/cxx.mk
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && "$(MARIO_CXX)" -std=c++17 -Wall -Wextra -Werror $(OPTIMIZE) \
		--mario-no-cxx-runtime -fno-exceptions -fno-rtti -fno-use-cxa-atexit "$<" -o "$@"

$(CXX_BUILD_DIR)/tests/cxx-stdlib.exe: $(CXX_DIR)/stdlib.cpp $(CXX_LINK_DEPS) $(CXX_RUNTIME_STAMP)
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && "$(MARIO_CXX)" -std=c++17 -Wall -Wextra -Werror $(OPTIMIZE) "$<" -o "$@"

clean:: clean-cxx
clean-cxx:
	$(RM) -r "$(CXX_BUILD_DIR)" "$(CXX_INSTALL_DIR)"

clean-src:: clean-src-cxx
clean-src-cxx:
	$(RM) -r "$(CXX_DIR)/downloads"
