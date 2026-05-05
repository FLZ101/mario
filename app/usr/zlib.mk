ZLIB_VER		:=1.3.2
ZLIB_SRC_DIR	:=zlib-$(ZLIB_VER)
ZLIB_SRC_FILE	:=$(ZLIB_SRC_DIR).tar.gz
ZLIB_SRC_URL	:=https://github.com/madler/zlib/releases/download/v$(ZLIB_VER)/$(ZLIB_SRC_FILE)
ZLIB_INSTALL_DIR	:=$(ROOT)/$(ZLIB_SRC_DIR)-install
export ZLIB_INSTALL_DIR

.PHONY build:: build-zlib
build-zlib: src-zlib
	CC=$(CC) ZLIB_SRC_DIR=$(ZLIB_SRC_DIR) ZLIB_INSTALL_DIR=$(ZLIB_INSTALL_DIR) ./build-zlib.sh

src-zlib: $(ZLIB_SRC_DIR)/configure

$(ZLIB_SRC_DIR)/configure: | $(ZLIB_SRC_FILE)
	tar xf $(ZLIB_SRC_FILE)

$(ZLIB_SRC_FILE):
	wget -c --no-check-certificate $(ZLIB_SRC_URL) -O $(ZLIB_SRC_FILE)

.PHONY clean:: clean-zlib
clean-zlib:
	$(RM) -r $(ZLIB_INSTALL_DIR) $(ZLIB_SRC_DIR)

.PHONY clean-src:: clean-src-zlib
clean-src-zlib:
	$(RM) $(ZLIB_SRC_FILE)
