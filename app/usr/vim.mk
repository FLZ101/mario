VIM_VER		:=9.2.0428
VIM_SRC_DIR	:=vim-$(VIM_VER)
VIM_SRC_FILE	:=$(VIM_SRC_DIR).tar.gz
VIM_SRC_URL	:=https://github.com/vim/vim/archive/refs/tags/v$(VIM_VER).tar.gz
VIM_INSTALL_DIR	:=$(ROOT)/$(VIM_SRC_DIR)-install

.PHONY build:: build-vim
build-vim: src-vim
	CC=$(CC) VIM_SRC_DIR=$(VIM_SRC_DIR) VIM_INSTALL_DIR=$(VIM_INSTALL_DIR) ./build-vim.sh

src-vim: $(VIM_SRC_DIR)/configure

$(VIM_SRC_DIR)/configure: | $(VIM_SRC_FILE)
	tar xf $(VIM_SRC_FILE)

$(VIM_SRC_FILE):
	wget -c --no-check-certificate $(VIM_SRC_URL) -O $(VIM_SRC_FILE)

.PHONY clean:: clean-vim
clean-vim:
	$(RM) -r $(VIM_INSTALL_DIR) $(VIM_SRC_DIR)

.PHONY clean-src:: clean-src-vim
clean-src-vim:
	$(RM) $(VIM_SRC_FILE)
