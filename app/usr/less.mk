LESS_VER		:=692
LESS_SRC_DIR	:=less-$(LESS_VER)
LESS_SRC_FILE	:=$(LESS_SRC_DIR).tar.gz
LESS_SRC_URL	:=https://ftp.gnu.org/gnu/less/$(LESS_SRC_FILE)
LESS_INSTALL_DIR	:=$(ROOT)/$(LESS_SRC_DIR)-install

.PHONY build:: build-less
build-less: src-less
	CC=$(CC) LESS_SRC_DIR=$(LESS_SRC_DIR) LESS_INSTALL_DIR=$(LESS_INSTALL_DIR) ./build-less.sh

src-less: $(LESS_SRC_DIR)/configure

$(LESS_SRC_DIR)/configure: | $(LESS_SRC_FILE)
	tar xf $(LESS_SRC_FILE)

$(LESS_SRC_FILE):
	wget -c --no-check-certificate $(LESS_SRC_URL) -O $(LESS_SRC_FILE)

.PHONY clean:: clean-less
clean-less:
	$(RM) -r $(LESS_INSTALL_DIR) $(LESS_SRC_DIR)

.PHONY clean-src:: clean-src-less
clean-src-less:
	$(RM) $(LESS_SRC_FILE)
