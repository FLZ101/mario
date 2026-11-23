OKSH_VER		:=7.8
OKSH_SRC_DIR	:=oksh-$(OKSH_VER)
OKSH_SRC_FILE	:=$(OKSH_SRC_DIR).tar.gz
OKSH_SRC_URL	:=https://github.com/ibara/oksh/releases/download/oksh-$(OKSH_VER)/$(OKSH_SRC_FILE)
OKSH_INSTALL_DIR	:=$(ROOT)/$(OKSH_SRC_DIR)-install
OKSH_PATCH		:=oksh.patch

.PHONY build:: build-oksh
build-oksh: $(OKSH_INSTALL_DIR)/.built

src-oksh: $(OKSH_SRC_DIR)/.prepared

$(OKSH_SRC_DIR)/.prepared: $(OKSH_SRC_FILE) $(OKSH_PATCH) oksh.mk
	$(RM) -r $(OKSH_SRC_DIR)
	tar --no-same-owner -xf $(OKSH_SRC_FILE)
	patch -d $(OKSH_SRC_DIR) -p1 < $(OKSH_PATCH)
	touch $@

$(OKSH_INSTALL_DIR)/.built: $(OKSH_SRC_DIR)/.prepared build-oksh.sh
	$(RM) -r $(OKSH_INSTALL_DIR)
	CC=$(CC) OKSH_SRC_DIR=$(OKSH_SRC_DIR) OKSH_INSTALL_DIR=$(OKSH_INSTALL_DIR) ./build-oksh.sh
	touch $@

$(OKSH_SRC_FILE):
	wget -c --no-check-certificate $(OKSH_SRC_URL) -O $(OKSH_SRC_FILE)

.PHONY clean:: clean-oksh
clean-oksh:
	$(RM) -r $(OKSH_INSTALL_DIR) $(OKSH_SRC_DIR)

.PHONY clean-src:: clean-src-oksh
clean-src-oksh:
	$(RM) $(OKSH_SRC_FILE)
