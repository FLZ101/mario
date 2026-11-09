LIBPNG_VER		:=1.6.59
LIBPNG_SRC_DIR		:=libpng-$(LIBPNG_VER)
LIBPNG_SRC_FILE		:=$(LIBPNG_SRC_DIR).tar.gz
LIBPNG_SRC_URL		:=https://download.sourceforge.net/libpng/$(LIBPNG_SRC_FILE)
LIBPNG_SRC_SHA256	:=86a3e4b501f7f50e392c4e456ea158893e9a595b1c63eb88c7bb9f6cf8772dad
LIBPNG_INSTALL_DIR	:=$(ROOT)/$(LIBPNG_SRC_DIR)-install
export LIBPNG_INSTALL_DIR

# libpng is libpng-2.0 licensed; the verified source archive includes LICENSE.

.PHONY build:: build-libpng
build-libpng: src-libpng build-zlib
	CC=$(CC) LIBPNG_SRC_DIR=$(LIBPNG_SRC_DIR) \
		LIBPNG_INSTALL_DIR=$(LIBPNG_INSTALL_DIR) \
		ZLIB_INSTALL_DIR=$(ZLIB_INSTALL_DIR) ./build-libpng.sh

src-libpng: $(LIBPNG_SRC_DIR)/configure

$(LIBPNG_SRC_DIR)/configure: | $(LIBPNG_SRC_FILE)
	@printf '%s  %s\n' '$(LIBPNG_SRC_SHA256)' '$(LIBPNG_SRC_FILE)' | sha256sum -c -
	tar --no-same-owner -xf $(LIBPNG_SRC_FILE)

$(LIBPNG_SRC_FILE):
	wget -c --no-check-certificate $(LIBPNG_SRC_URL) -O $(LIBPNG_SRC_FILE)

.PHONY clean:: clean-libpng
clean-libpng:
	$(RM) -r $(LIBPNG_INSTALL_DIR) $(LIBPNG_SRC_DIR)

.PHONY clean-src:: clean-src-libpng
clean-src-libpng:
	$(RM) $(LIBPNG_SRC_FILE)
