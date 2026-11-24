CXX_DIR := $(ROOT)/cxx
MARIO_CXX := $(abspath $(ROOT)/../../util/mario-musl-c++)
CXX_LIBC := $(abspath $(ROOT)/../../musl/musl-1.2.5-install/lib/libc.a)
CXX_LINK_DEPS := $(MARIO_CXX) $(CXX_LIBC) $(ROOT)/../../musl/musl.ld \
	$(ROOT)/../../compiler-rt/libcompiler_rt.a
CXX_BUILD_DIR := $(CXX_DIR)/build

.PHONY:: build-cxx-probes clean-cxx
build:: build-cxx-probes
build-cxx-probes: $(CXX_BUILD_DIR)/tests/cxx-startup.exe

$(CXX_BUILD_DIR)/tests/cxx-startup.exe: $(CXX_DIR)/startup.cpp $(CXX_LINK_DEPS) $(CXX_DIR)/cxx.mk
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && "$(MARIO_CXX)" -std=c++17 -Wall -Wextra -Werror $(OPTIMIZE) \
		-fno-exceptions -fno-rtti -fno-use-cxa-atexit "$<" -o "$@"

clean:: clean-cxx
clean-cxx:
	$(RM) -r "$(CXX_BUILD_DIR)"
