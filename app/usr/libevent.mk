LIBEVENT_VER		:=2.1.13-stable
LIBEVENT_SRC_DIR	:=libevent-$(LIBEVENT_VER)
LIBEVENT_SRC_FILE	:=$(LIBEVENT_SRC_DIR).tar.gz
LIBEVENT_SRC_URL	:=https://github.com/libevent/libevent/releases/download/release-$(LIBEVENT_VER)/$(LIBEVENT_SRC_FILE)
LIBEVENT_INSTALL_DIR	:=$(ROOT)/$(LIBEVENT_SRC_DIR)-install
export LIBEVENT_INSTALL_DIR

.PHONY build:: build-libevent
build-libevent: src-libevent
	CC=$(CC) LIBEVENT_SRC_DIR=$(LIBEVENT_SRC_DIR) LIBEVENT_INSTALL_DIR=$(LIBEVENT_INSTALL_DIR) ./build-libevent.sh

.PHONY:: src-libevent
src-libevent: $(LIBEVENT_SRC_DIR)/configure

$(LIBEVENT_SRC_DIR)/configure: | $(LIBEVENT_SRC_FILE)
	tar --no-same-owner -xf $(LIBEVENT_SRC_FILE)

$(LIBEVENT_SRC_FILE):
	wget -c --no-check-certificate $(LIBEVENT_SRC_URL) -O $(LIBEVENT_SRC_FILE)

.PHONY clean:: clean-libevent
clean-libevent:
	$(RM) -r $(LIBEVENT_INSTALL_DIR) $(LIBEVENT_SRC_DIR)

.PHONY clean-src:: clean-src-libevent
clean-src-libevent:
	$(RM) $(LIBEVENT_SRC_FILE)
