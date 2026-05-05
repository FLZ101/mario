READLINE_VER		:=8.3
READLINE_SRC_DIR	:=readline-$(READLINE_VER)
READLINE_SRC_FILE	:=$(READLINE_SRC_DIR).tar.gz
READLINE_SRC_URL	:=https://ftp.gnu.org/gnu/readline/$(READLINE_SRC_FILE)
READLINE_INSTALL_DIR	:=$(ROOT)/$(READLINE_SRC_DIR)-install
export READLINE_INSTALL_DIR

.PHONY build:: build-readline
build-readline: src-readline
	CC=$(CC) READLINE_SRC_DIR=$(READLINE_SRC_DIR) READLINE_INSTALL_DIR=$(READLINE_INSTALL_DIR) ./build-readline.sh

src-readline: $(READLINE_SRC_DIR)/configure

$(READLINE_SRC_DIR)/configure: | $(READLINE_SRC_FILE)
	tar xf $(READLINE_SRC_FILE)

$(READLINE_SRC_FILE):
	wget -c --no-check-certificate $(READLINE_SRC_URL) -O $(READLINE_SRC_FILE)

.PHONY clean:: clean-readline
clean-readline:
	$(RM) -r $(READLINE_INSTALL_DIR) $(READLINE_SRC_DIR)

.PHONY clean-src:: clean-src-readline
clean-src-readline:
	$(RM) $(READLINE_SRC_FILE)
