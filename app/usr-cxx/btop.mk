BTOP_VER := 1.4.7
BTOP_SRC_DIR := $(ROOT)/btop-$(BTOP_VER)
BTOP_SRC_FILE := btop-$(BTOP_VER).tar.gz
BTOP_SRC_URL := https://codeload.github.com/aristocratos/btop/tar.gz/refs/tags/v$(BTOP_VER)
BTOP_SHA256 := 933de2e4d1b2211a638be463eb6e8616891bfba73aef5d38060bd8319baeefc6
BTOP_INSTALL_DIR := $(BTOP_SRC_DIR)-install
BTOP_BINARY := $(BTOP_INSTALL_DIR)/bin/btop
BTOP_LICENSE := $(BTOP_INSTALL_DIR)/share/doc/btop/LICENSE
BTOP_THEMES := $(BTOP_INSTALL_DIR)/share/btop/themes
BTOP_TOOLCHAIN := $(abspath $(ROOT)/../usr/cxx/build/.toolchain-stamp)
BTOP_RULES := $(lastword $(MAKEFILE_LIST)) btop.patch build-btop.sh

.PHONY:: build-btop src-btop clean-btop clean-src-btop
build:: build-btop
build-btop: $(BTOP_BINARY) $(BTOP_LICENSE) $(BTOP_THEMES)

src-btop: $(BTOP_SRC_DIR)/.prepared

$(BTOP_SRC_DIR)/.prepared: $(BTOP_SRC_FILE) $(BTOP_RULES) $(BTOP_TOOLCHAIN)
	printf '%s  %s\n' '$(BTOP_SHA256)' "$<" | sha256sum -c -
	$(RM) -r "$(BTOP_SRC_DIR)"
	tar xf "$<"
	patch -d "$(BTOP_SRC_DIR)" -p1 < btop.patch
	touch "$@"

$(BTOP_SRC_FILE):
	wget --tries=3 '$(BTOP_SRC_URL)' -O "$@.tmp"
	printf '%s  %s\n' '$(BTOP_SHA256)' "$@.tmp" | sha256sum -c -
	mv "$@.tmp" "$@"

$(BTOP_BINARY) $(BTOP_LICENSE) $(BTOP_THEMES) &: $(BTOP_SRC_DIR)/.prepared
	CXX="$(CXX)" PYTHON="$(CXX_PYTHON)" BTOP_SRC_DIR="$(BTOP_SRC_DIR)" \
		BTOP_INSTALL_DIR="$(BTOP_INSTALL_DIR)" ./build-btop.sh

clean:: clean-btop
clean-btop:
	$(RM) -r "$(BTOP_SRC_DIR)" "$(BTOP_INSTALL_DIR)"

clean-src:: clean-src-btop
clean-src-btop:
	$(RM) "$(BTOP_SRC_FILE)" "$(BTOP_SRC_FILE).tmp"
