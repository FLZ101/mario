OPENSSL_VER		:=3.5.9
OPENSSL_SRC_DIR		:=openssl-$(OPENSSL_VER)
OPENSSL_SRC_FILE	:=$(OPENSSL_SRC_DIR).tar.gz
OPENSSL_SRC_URL		:=https://www.openssl.org/source/$(OPENSSL_SRC_FILE)
OPENSSL_SRC_SHA256	:=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
OPENSSL_INSTALL_DIR	:=$(ROOT)/$(OPENSSL_SRC_DIR)-install
export OPENSSL_INSTALL_DIR

.PHONY build:: build-openssl
build-openssl: $(OPENSSL_INSTALL_DIR)/.mario-build

$(OPENSSL_INSTALL_DIR)/.mario-build: $(OPENSSL_SRC_DIR)/Configure build-openssl.sh openssl.mk
	CC=$(CC) OPENSSL_SRC_DIR=$(OPENSSL_SRC_DIR) OPENSSL_INSTALL_DIR=$(OPENSSL_INSTALL_DIR) ./build-openssl.sh

$(OPENSSL_SRC_DIR)/Configure: | $(OPENSSL_SRC_FILE)
	tar xf $(OPENSSL_SRC_FILE)

$(OPENSSL_SRC_FILE):
	curl --fail --location --output $@.tmp $(OPENSSL_SRC_URL)
	printf '%s  %s\n' $(OPENSSL_SRC_SHA256) $@.tmp | sha256sum --check
	mv $@.tmp $@

.PHONY clean:: clean-openssl
clean-openssl:
	$(RM) -r $(OPENSSL_INSTALL_DIR) $(OPENSSL_SRC_DIR)

.PHONY clean-src:: clean-src-openssl
clean-src-openssl:
	$(RM) $(OPENSSL_SRC_FILE) $(OPENSSL_SRC_FILE).tmp
