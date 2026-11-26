TOYBOX_VER		:=0.8.9
TOYBOX_SRC_DIR	:=toybox-$(TOYBOX_VER)
TOYBOX_SRC_FILE	:=$(TOYBOX_SRC_DIR).tar.gz
# TOYBOX_SRC_URL	:=https://landley.net/toybox/downloads/$(TOYBOX_SRC_FILE)
TOYBOX_SRC_URL	:=https://github.com/landley/toybox/archive/refs/tags/$(TOYBOX_VER).tar.gz
TOYBOX_INSTALL_DIR	:=$(ROOT)/$(TOYBOX_SRC_DIR)-install

.PHONY build:: build-toybox
build-toybox: $(TOYBOX_INSTALL_DIR)/bin/toybox

$(TOYBOX_INSTALL_DIR)/bin/toybox: $(TOYBOX_SRC_FILE) toybox/.config toybox.patch build-toybox.sh toybox.mk
	CC=$(CC) TOYBOX_SRC_DIR=$(TOYBOX_SRC_DIR) TOYBOX_SRC_FILE=$(TOYBOX_SRC_FILE) \
		TOYBOX_INSTALL_DIR=$(TOYBOX_INSTALL_DIR) ./build-toybox.sh

$(TOYBOX_SRC_FILE):
	wget -c --no-check-certificate $(TOYBOX_SRC_URL) -O $(TOYBOX_SRC_FILE)

.PHONY clean:: clean-toybox
clean-toybox:
	$(RM) -r $(TOYBOX_INSTALL_DIR) $(TOYBOX_SRC_DIR)

.PHONY clean-src:: clean-src-toybox
clean-src-toybox:
	$(RM) $(TOYBOX_SRC_FILE)
