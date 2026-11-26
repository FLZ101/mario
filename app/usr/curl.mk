CURL_VER		:=8.23.0
CURL_SRC_DIR		:=curl-$(CURL_VER)
CURL_SRC_FILE		:=$(CURL_SRC_DIR).tar.xz
CURL_SRC_URL		:=https://curl.se/download/$(CURL_SRC_FILE)
CURL_SRC_SHA256		:=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
CURL_INSTALL_DIR	:=$(ROOT)/$(CURL_SRC_DIR)-install

.PHONY build:: build-curl
build-curl: $(CURL_INSTALL_DIR)/bin/curl

$(CURL_INSTALL_DIR)/bin/curl: $(CURL_SRC_DIR)/configure $(OPENSSL_INSTALL_DIR)/.mario-build build-curl.sh curl.mk
	CC=$(CC) CURL_SRC_DIR=$(CURL_SRC_DIR) CURL_INSTALL_DIR=$(CURL_INSTALL_DIR) ./build-curl.sh

$(CURL_SRC_DIR)/configure: | $(CURL_SRC_FILE)
	tar xf $(CURL_SRC_FILE)

$(CURL_SRC_FILE):
	curl --fail --location --output $@.tmp $(CURL_SRC_URL)
	printf '%s  %s\n' $(CURL_SRC_SHA256) $@.tmp | sha256sum --check
	mv $@.tmp $@

.PHONY clean:: clean-curl
clean-curl:
	$(RM) -r $(CURL_INSTALL_DIR) $(CURL_SRC_DIR)

.PHONY clean-src:: clean-src-curl
clean-src-curl:
	$(RM) $(CURL_SRC_FILE) $(CURL_SRC_FILE).tmp
