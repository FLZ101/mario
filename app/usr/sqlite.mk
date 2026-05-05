SQLITE_SRC_DIR	:=sqlite-autoconf-3530000
SQLITE_SRC_FILE	:=$(SQLITE_SRC_DIR).tar.gz
SQLITE_SRC_URL	:=https://sqlite.org/2026/$(SQLITE_SRC_FILE)
SQLITE_INSTALL_DIR	:=$(ROOT)/sqlite-install
export SQLITE_INSTALL_DIR

.PHONY build:: build-sqlite
build-sqlite: src-sqlite
	CC=$(CC) SQLITE_SRC_DIR=$(SQLITE_SRC_DIR) SQLITE_INSTALL_DIR=$(SQLITE_INSTALL_DIR) ./build-sqlite.sh

src-sqlite: $(SQLITE_SRC_DIR)/configure

$(SQLITE_SRC_DIR)/configure: | $(SQLITE_SRC_FILE)
	tar xf $(SQLITE_SRC_FILE)

$(SQLITE_SRC_FILE):
	wget -c --no-check-certificate $(SQLITE_SRC_URL) -O $(SQLITE_SRC_FILE)

.PHONY clean:: clean-sqlite
clean-sqlite:
	$(RM) -r $(SQLITE_INSTALL_DIR) $(SQLITE_SRC_DIR)

.PHONY clean-src:: clean-src-sqlite
clean-src-sqlite:
	$(RM) $(SQLITE_SRC_FILE)
