WGET_VER		:=1.25.0
WGET_SRC_DIR		:=wget-$(WGET_VER)
WGET_SRC_FILE		:=$(WGET_SRC_DIR).tar.gz
WGET_SRC_URL		:=https://mirrors.kernel.org/gnu/wget/$(WGET_SRC_FILE)
WGET_SRC_SHA256		:=766e48423e79359ea31e41db9e5c289675947a7fcf2efdcedb726ac9d0da3784
WGET_INSTALL_DIR	:=$(ROOT)/$(WGET_SRC_DIR)-install

.PHONY build:: build-wget
build-wget: $(WGET_INSTALL_DIR)/bin/wget

$(WGET_INSTALL_DIR)/bin/wget: $(WGET_SRC_DIR)/configure $(OPENSSL_INSTALL_DIR)/.mario-build build-wget.sh wget.mk
	CC=$(CC) WGET_SRC_DIR=$(WGET_SRC_DIR) WGET_INSTALL_DIR=$(WGET_INSTALL_DIR) ./build-wget.sh

$(WGET_SRC_DIR)/configure: | $(WGET_SRC_FILE)
	tar xf $(WGET_SRC_FILE)

$(WGET_SRC_FILE):
	curl --fail --location --output $@.tmp $(WGET_SRC_URL)
	printf '%s  %s\n' $(WGET_SRC_SHA256) $@.tmp | sha256sum --check
	mv $@.tmp $@

.PHONY clean:: clean-wget
clean-wget:
	$(RM) -r $(WGET_INSTALL_DIR) $(WGET_SRC_DIR)

.PHONY clean-src:: clean-src-wget
clean-src-wget:
	$(RM) $(WGET_SRC_FILE) $(WGET_SRC_FILE).tmp
