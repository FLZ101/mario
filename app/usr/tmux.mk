TMUX_VER		:=3.8
TMUX_SRC_DIR	:=tmux-$(TMUX_VER)
TMUX_SRC_FILE	:=$(TMUX_SRC_DIR).tar.gz
TMUX_SRC_URL	:=https://github.com/tmux/tmux/releases/download/$(TMUX_VER)/$(TMUX_SRC_FILE)
TMUX_INSTALL_DIR	:=$(ROOT)/$(TMUX_SRC_DIR)-install

.PHONY build:: build-tmux
build-tmux: build-libevent src-tmux
	CC=$(CC) TMUX_SRC_DIR=$(TMUX_SRC_DIR) TMUX_INSTALL_DIR=$(TMUX_INSTALL_DIR) LIBEVENT_INSTALL_DIR=$(LIBEVENT_INSTALL_DIR) NCURSES_INSTALL_DIR=$(NCURSES_INSTALL_DIR) ./build-tmux.sh

.PHONY:: src-tmux
src-tmux: $(TMUX_SRC_DIR)/configure

$(TMUX_SRC_DIR)/configure: | $(TMUX_SRC_FILE)
	tar --no-same-owner -xf $(TMUX_SRC_FILE)

$(TMUX_SRC_FILE):
	wget -c --no-check-certificate $(TMUX_SRC_URL) -O $(TMUX_SRC_FILE)

.PHONY clean:: clean-tmux
clean-tmux:
	$(RM) -r $(TMUX_INSTALL_DIR) $(TMUX_SRC_DIR)

.PHONY clean-src:: clean-src-tmux
clean-src-tmux:
	$(RM) $(TMUX_SRC_FILE)
