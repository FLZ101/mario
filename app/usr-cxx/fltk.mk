FLTK_VER := 1.4.5
FLTK_SRC_DIR := $(ROOT)/fltk-$(FLTK_VER)
FLTK_SRC_FILE := fltk-$(FLTK_VER).tar.gz
FLTK_SRC_URL := https://codeload.github.com/fltk/fltk/tar.gz/refs/tags/release-$(FLTK_VER)
FLTK_SHA256 := 7715e69ce081fa9ce6da48bb0dd3b07a4cf2cf937813814c04272f36fff593ea
FLTK_BUILD_DIR := $(FLTK_SRC_DIR)-build
FLTK_INSTALL_DIR := $(FLTK_SRC_DIR)-install
FLTK_OUTPUTS := $(FLTK_INSTALL_DIR)/lib/libfltk.a \
	$(FLTK_INSTALL_DIR)/include/FL/fl_config.h \
	$(FLTK_INSTALL_DIR)/share/doc/fltk/COPYING
FLTK_TOOLCHAIN := $(abspath $(ROOT)/../usr/cxx/build/.toolchain-stamp)
FLTK_RULES := $(lastword $(MAKEFILE_LIST)) fltk.patch build-fltk.sh

.PHONY:: build-fltk src-fltk clean-fltk clean-src-fltk
build:: build-fltk
build-fltk: $(FLTK_OUTPUTS)

src-fltk: $(FLTK_SRC_DIR)/.prepared

$(FLTK_SRC_DIR)/.prepared: $(FLTK_SRC_FILE) $(FLTK_RULES)
	printf '%s  %s\n' '$(FLTK_SHA256)' "$<" | sha256sum -c -
	$(RM) -r "$(FLTK_SRC_DIR)"
	mkdir -p "$(FLTK_SRC_DIR)"
	tar xf "$<" --strip-components=1 -C "$(FLTK_SRC_DIR)"
	patch -d "$(FLTK_SRC_DIR)" -p1 < fltk.patch
	touch "$@"

$(FLTK_SRC_FILE):
	curl -fL --retry 3 '$(FLTK_SRC_URL)' -o "$@.tmp"
	printf '%s  %s\n' '$(FLTK_SHA256)' "$@.tmp" | sha256sum -c -
	mv "$@.tmp" "$@"

$(FLTK_OUTPUTS) &: $(FLTK_SRC_DIR)/.prepared $(FLTK_TOOLCHAIN) $(CC) $(CXX) ../include/mario/input.h \
		$(wildcard fltk/*.cxx fltk/*.h ../gui/libmario-ui/include/gui/*.h)
	CC="$(CC)" CXX="$(CXX)" PYTHON="$(CXX_PYTHON)" \
		FLTK_SRC_DIR="$(FLTK_SRC_DIR)" FLTK_BUILD_DIR="$(FLTK_BUILD_DIR)" \
		FLTK_COMPILE_COMMANDS_JSON="$(FLTK_COMPILE_COMMANDS_JSON)" \
		FLTK_INSTALL_DIR="$(FLTK_INSTALL_DIR)" bash build-fltk.sh

clean:: clean-fltk
clean-fltk:
	$(RM) -r "$(FLTK_SRC_DIR)" "$(FLTK_BUILD_DIR)" "$(FLTK_INSTALL_DIR)"

clean-src:: clean-src-fltk
clean-src-fltk:
	$(RM) "$(FLTK_SRC_FILE)" "$(FLTK_SRC_FILE).tmp"
