NET_TOOLS_VER		:=2.10
NET_TOOLS_SRC_DIR	:=net-tools-$(NET_TOOLS_VER)
NET_TOOLS_SRC_FILE	:=$(NET_TOOLS_SRC_DIR).tar.xz
NET_TOOLS_SRC_URL	:=https://downloads.sourceforge.net/project/net-tools/$(NET_TOOLS_SRC_FILE)
NET_TOOLS_SRC_SHA256	:=b262435a5241e89bfa51c3cabd5133753952f7a7b7b93f32e08cb9d96f580d69
NET_TOOLS_INSTALL_DIR	:=$(ROOT)/$(NET_TOOLS_SRC_DIR)-install
NET_TOOLS_BINS		:=$(addprefix $(NET_TOOLS_INSTALL_DIR)/bin/,ifconfig route netstat)
NET_TOOLS_HEADERS	:=../include/mario/netctl.h ../../src/include/uapi/mario/netctl.h

.PHONY build:: build-net-tools
build-net-tools: $(NET_TOOLS_BINS)

$(NET_TOOLS_BINS) &: $(NET_TOOLS_SRC_FILE) nettools.patch nettools.c build-nettools.sh nettools.mk $(NET_TOOLS_HEADERS)
	CC=$(CC) NET_TOOLS_SRC_DIR=$(NET_TOOLS_SRC_DIR) NET_TOOLS_SRC_FILE=$(NET_TOOLS_SRC_FILE) \
		NET_TOOLS_INSTALL_DIR=$(NET_TOOLS_INSTALL_DIR) ./build-nettools.sh

$(NET_TOOLS_SRC_FILE):
	curl --fail --location --output $@.tmp $(NET_TOOLS_SRC_URL)
	printf '%s  %s\n' $(NET_TOOLS_SRC_SHA256) $@.tmp | sha256sum --check
	mv $@.tmp $@

.PHONY clean:: clean-net-tools
clean-net-tools:
	$(RM) -r $(NET_TOOLS_INSTALL_DIR) $(NET_TOOLS_SRC_DIR)

.PHONY clean-src:: clean-src-net-tools
clean-src-net-tools:
	$(RM) $(NET_TOOLS_SRC_FILE) $(NET_TOOLS_SRC_FILE).tmp
