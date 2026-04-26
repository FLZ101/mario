
UBASE_COMMIT_ID	:=f152e7fc3bd1675060818ac224f96541a2d9d6e7
UBASE_SRC_DIR	:=ubase-$(UBASE_COMMIT_ID)
UBASE_SRC_FILE	:=$(UBASE_SRC_DIR).zip
UBASE_SRC_URL	:=https://github.com/michaelforney/ubase/archive/$(UBASE_COMMIT_ID).zip

UBASE_INSTALL_DIR	:=$(ROOT)/ubase-install

.PHONY build:: build-ubase
build-ubase: src-ubase
	CC=$(CC) UBASE_SRC_DIR=$(UBASE_SRC_DIR) UBASE_INSTALL_DIR=$(UBASE_INSTALL_DIR) ./build-ubase.sh

src-ubase: $(UBASE_SRC_DIR)/README

$(UBASE_SRC_DIR)/README: | $(UBASE_SRC_FILE)
# 	tar xf $(UBASE_SRC_FILE)
	unzip $(UBASE_SRC_FILE)

$(UBASE_SRC_FILE):
	wget -c --no-check-certificate $(UBASE_SRC_URL) -O $(UBASE_SRC_FILE)

.PHONY clean:: clean-ubase
clean-ubase:
	$(RM) -r $(UBASE_INSTALL_DIR) $(UBASE_SRC_DIR)

.PHONY clean-src:: clean-src-ubase
clean-src-ubase:
	$(RM) $(UBASE_SRC_FILE)
