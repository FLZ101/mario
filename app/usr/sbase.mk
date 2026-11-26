# SBASE_VER		:=0.1
# SBASE_SRC_DIR	:=sbase-$(SBASE_VER)
# SBASE_SRC_FILE	:=$(SBASE_SRC_DIR).tar.gz
# SBASE_SRC_URL	:=https://dl.suckless.org/sbase/$(SBASE_SRC_FILE)

SBASE_COMMIT_ID	:=ac6d382515327bf0bf14d5d355d33ceffddc7fd7
SBASE_SRC_DIR	:=sbase-$(SBASE_COMMIT_ID)
SBASE_SRC_FILE	:=$(SBASE_SRC_DIR).zip
SBASE_SRC_URL	:=https://github.com/michaelforney/sbase/archive/$(SBASE_COMMIT_ID).zip

SBASE_INSTALL_DIR	:=$(ROOT)/sbase-install

.PHONY build:: build-sbase
build-sbase: $(SBASE_INSTALL_DIR)/bin/sbase-box

$(SBASE_INSTALL_DIR)/bin/sbase-box: $(SBASE_SRC_FILE) sbase.patch build-sbase.sh sbase.mk
	CC=$(CC) SBASE_SRC_DIR=$(SBASE_SRC_DIR) SBASE_SRC_FILE=$(SBASE_SRC_FILE) \
		SBASE_INSTALL_DIR=$(SBASE_INSTALL_DIR) ./build-sbase.sh

$(SBASE_SRC_FILE):
	wget -c --no-check-certificate $(SBASE_SRC_URL) -O $(SBASE_SRC_FILE)

.PHONY clean:: clean-sbase
clean-sbase:
	$(RM) -r $(SBASE_INSTALL_DIR) $(SBASE_SRC_DIR)

.PHONY clean-src:: clean-src-sbase
clean-src-sbase:
	$(RM) $(SBASE_SRC_FILE)
