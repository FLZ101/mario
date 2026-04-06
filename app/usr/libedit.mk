LIBEDIT_VER		:=20251016-3.1
LIBEDIT_SRC_DIR	:=libedit-$(LIBEDIT_VER)
LIBEDIT_SRC_FILE	:=$(LIBEDIT_SRC_DIR).tar.gz
LIBEDIT_SRC_URL	:=https://www.thrysoee.dk/editline/$(LIBEDIT_SRC_FILE)
LIBEDIT_INSTALL_DIR	:=$(ROOT)/$(LIBEDIT_SRC_DIR)-install
export LIBEDIT_INSTALL_DIR

.PHONY build:: build-libedit
build-libedit: src-libedit
	CC=$(CC) LIBEDIT_SRC_DIR=$(LIBEDIT_SRC_DIR) LIBEDIT_INSTALL_DIR=$(LIBEDIT_INSTALL_DIR) ./build-libedit.sh

src-libedit: $(LIBEDIT_SRC_DIR)/autogen.sh

$(LIBEDIT_SRC_DIR)/autogen.sh: $(LIBEDIT_SRC_FILE)
	tar xf $(LIBEDIT_SRC_FILE)
	touch $(LIBEDIT_SRC_DIR)/autogen.sh # Update timestamp

$(LIBEDIT_SRC_FILE):
	wget -c --no-check-certificate $(LIBEDIT_SRC_URL) -O $(LIBEDIT_SRC_FILE)

.PHONY clean:: clean-libedit
clean-libedit:
	$(RM) -r $(LIBEDIT_INSTALL_DIR) $(LIBEDIT_SRC_DIR)

.PHONY clean-src:: clean-src-libedit
clean-src-libedit:
	$(RM) $(LIBEDIT_SRC_FILE)
