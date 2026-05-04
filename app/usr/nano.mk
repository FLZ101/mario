NANO_VER		:=9.0
NANO_SRC_DIR	:=nano-$(NANO_VER)
NANO_SRC_FILE	:=$(NANO_SRC_DIR).tar.gz
NANO_SRC_URL	:=https://ftp.gnu.org/gnu/nano/$(NANO_SRC_FILE)
NANO_INSTALL_DIR	:=$(ROOT)/$(NANO_SRC_DIR)-install

.PHONY build:: build-nano
build-nano: src-nano
	CC=$(CC) NANO_SRC_DIR=$(NANO_SRC_DIR) NANO_INSTALL_DIR=$(NANO_INSTALL_DIR) ./build-nano.sh

src-nano: $(NANO_SRC_DIR)/configure

$(NANO_SRC_DIR)/configure: | $(NANO_SRC_FILE)
	tar xf $(NANO_SRC_FILE)

$(NANO_SRC_FILE):
	wget -c --no-check-certificate $(NANO_SRC_URL) -O $(NANO_SRC_FILE)

.PHONY clean:: clean-nano
clean-nano:
	$(RM) -r $(NANO_INSTALL_DIR) $(NANO_SRC_DIR)

.PHONY clean-src:: clean-src-nano
clean-src-nano:
	$(RM) $(NANO_SRC_FILE)
