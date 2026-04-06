NCURSES_VER		:=6.6
NCURSES_SRC_DIR	:=ncurses-$(NCURSES_VER)
NCURSES_SRC_FILE	:=$(NCURSES_SRC_DIR).tar.gz
NCURSES_SRC_URL	:=https://invisible-island.net/archives/ncurses/$(NCURSES_SRC_FILE)
NCURSES_INSTALL_DIR	:=$(ROOT)/$(NCURSES_SRC_DIR)-install
export NCURSES_INSTALL_DIR

.PHONY build:: build-ncurses
build-ncurses: src-ncurses
	CC=$(CC) NCURSES_SRC_DIR=$(NCURSES_SRC_DIR) NCURSES_INSTALL_DIR=$(NCURSES_INSTALL_DIR) ./build-ncurses.sh

src-ncurses: $(NCURSES_SRC_DIR)/autogen.sh

$(NCURSES_SRC_DIR)/autogen.sh: $(NCURSES_SRC_FILE)
	tar xf $(NCURSES_SRC_FILE)
	touch $(NCURSES_SRC_DIR)/autogen.sh

$(NCURSES_SRC_FILE):
	wget -c --no-check-certificate $(NCURSES_SRC_URL) -O $(NCURSES_SRC_FILE)

.PHONY clean:: clean-ncurses
clean-ncurses:
	$(RM) -r $(NCURSES_INSTALL_DIR) $(NCURSES_SRC_DIR)

.PHONY clean-src:: clean-src-ncurses
clean-src-ncurses:
	$(RM) $(NCURSES_SRC_FILE)
