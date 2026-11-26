OPENSSH_VER		:=10.6p2
OPENSSH_SRC_DIR		:=openssh-$(OPENSSH_VER)
OPENSSH_SRC_FILE	:=$(OPENSSH_SRC_DIR).tar.gz
OPENSSH_SRC_URL		:=https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/$(OPENSSH_SRC_FILE)
OPENSSH_SRC_SHA256	:=9ae44872090cf2f06b3cf9ff147e7f16f309671340763ed2f99248398ae29d5b
OPENSSH_INSTALL_DIR	:=$(ROOT)/$(OPENSSH_SRC_DIR)-install
OPENSSH_CLIENT_BINS	:=$(addprefix $(OPENSSH_INSTALL_DIR)/bin/,ssh ssh-keygen scp sftp)
OPENSSH_SERVER_BINS	:= \
	$(OPENSSH_INSTALL_DIR)/sbin/sshd \
	$(addprefix $(OPENSSH_INSTALL_DIR)/libexec/,sshd-session sshd-auth sftp-server)
OPENSSH_BINS		:=$(OPENSSH_CLIENT_BINS) $(OPENSSH_SERVER_BINS)

.PHONY build:: build-openssh
build-openssh: $(OPENSSH_BINS)

$(OPENSSH_BINS) &: $(OPENSSH_SRC_DIR)/configure $(OPENSSL_INSTALL_DIR)/.mario-build \
		build-openssh.sh openssh.mk zlib.mk | build-zlib
	CC=$(CC) OPENSSH_SRC_DIR=$(OPENSSH_SRC_DIR) OPENSSH_INSTALL_DIR=$(OPENSSH_INSTALL_DIR) ./build-openssh.sh

$(OPENSSH_SRC_DIR)/configure: | $(OPENSSH_SRC_FILE)
	tar xf $(OPENSSH_SRC_FILE)

$(OPENSSH_SRC_FILE):
	curl --fail --location --output $@.tmp $(OPENSSH_SRC_URL)
	printf '%s  %s\n' $(OPENSSH_SRC_SHA256) $@.tmp | sha256sum --check
	mv $@.tmp $@

.PHONY clean:: clean-openssh
clean-openssh:
	$(RM) -r $(OPENSSH_INSTALL_DIR) $(OPENSSH_SRC_DIR)

.PHONY clean-src:: clean-src-openssh
clean-src-openssh:
	$(RM) $(OPENSSH_SRC_FILE) $(OPENSSH_SRC_FILE).tmp
