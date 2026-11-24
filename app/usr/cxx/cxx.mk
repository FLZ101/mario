include cxx/runtime.mk

.PHONY:: build-cxx-runtime build-cxx-probes clean-cxx clean-src-cxx
build:: build-cxx-probes
build-cxx-runtime: $(CXX_RUNTIME_STAMP)
build-cxx-probes: $(addprefix $(CXX_BUILD_DIR)/tests/,cxx-startup.exe cxx-stdlib.exe cxx-exceptions.exe)

$(CXX_BUILD_DIR)/tests/cxx-startup.exe: $(CXX_DIR)/startup.cpp $(CXX_LINK_DEPS) $(CXX_DIR)/cxx.mk
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && $(MARIO_CXX_CMD) -std=c++17 -Wall -Wextra -Werror $(OPTIMIZE) \
		--mario-no-cxx-runtime -fno-exceptions -fno-rtti -fno-use-cxa-atexit "$<" -o "$@"

$(CXX_BUILD_DIR)/tests/cxx-stdlib.exe: $(CXX_DIR)/stdlib.cpp $(CXX_LINK_DEPS) $(CXX_RUNTIME_STAMP)
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && $(MARIO_CXX_CMD) -std=c++17 -Wall -Wextra -Werror $(OPTIMIZE) "$<" -o "$@"

CXX_EXCEPTION_SOURCES := $(addprefix $(CXX_DIR)/,exceptions.cpp exceptions-throw.cpp)
$(CXX_BUILD_DIR)/tests/cxx-exceptions.exe: $(CXX_EXCEPTION_SOURCES) $(CXX_LINK_DEPS) $(CXX_RUNTIME_STAMP)
	mkdir -p "$(@D)"
	cd "$(CXX_BUILD_DIR)" && $(MARIO_CXX_CMD) -std=c++17 -Wall -Wextra -Werror $(OPTIMIZE) $(CXX_EXCEPTION_SOURCES) -o "$@"

clean:: clean-cxx
clean-cxx:
	$(RM) -r "$(CXX_BUILD_DIR)" "$(CXX_INSTALL_DIR)"

clean-src:: clean-src-cxx
clean-src-cxx:
	$(RM) -r "$(CXX_DIR)/downloads"
