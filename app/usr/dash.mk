DASH_VER		:=0.5.13
DASH_SRC_DIR	:=dash-$(DASH_VER)
DASH_SRC_FILE	:=$(DASH_SRC_DIR).tar.gz
DASH_SRC_URL	:=https://git.kernel.org/pub/scm/utils/dash/dash.git/snapshot/$(DASH_SRC_FILE)
DASH_INSTALL_DIR	:=$(ROOT)/$(DASH_SRC_DIR)-install

.PHONY build:: build-dash
build-dash: src-dash
	CC=$(CC) DASH_SRC_DIR=$(DASH_SRC_DIR) DASH_INSTALL_DIR=$(DASH_INSTALL_DIR) ./build-dash.sh

src-dash: $(DASH_SRC_DIR)/autogen.sh

$(DASH_SRC_DIR)/autogen.sh: | $(DASH_SRC_FILE)
	tar xf $(DASH_SRC_FILE)

$(DASH_SRC_FILE):
	wget -c --no-check-certificate $(DASH_SRC_URL) -O $(DASH_SRC_FILE)

.PHONY clean:: clean-dash
clean-dash:
	$(RM) -r $(DASH_INSTALL_DIR) $(DASH_SRC_DIR)

.PHONY clean-src:: clean-src-dash
clean-src-dash:
	$(RM) $(DASH_SRC_FILE)
