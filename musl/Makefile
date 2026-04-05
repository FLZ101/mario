RM	:=rm -f

ROOT := $(shell pwd)

MUSL_VER		:=1.2.5
MUSL_SRC_DIR	:=musl-$(MUSL_VER)
MUSL_SRC_FILE	:=$(MUSL_SRC_DIR).tar.gz
MUSL_SRC_URL	:=https://musl.libc.org/releases/$(MUSL_SRC_FILE)
MUSL_INSTALL_DIR	:=$(ROOT)/$(MUSL_SRC_DIR)-install

.PHONY: all build src check
.PHONY: clean clean-src

all: build

build: src | check
	CC=$(CC) MUSL_SRC_DIR=$(MUSL_SRC_DIR) MUSL_INSTALL_DIR=$(MUSL_INSTALL_DIR) ./build-musl.sh

check:
	@$(CC) --mario-verify

src: $(MUSL_SRC_DIR)/VERSION

$(MUSL_SRC_DIR)/VERSION: | $(MUSL_SRC_FILE)
	tar xf $(MUSL_SRC_FILE)

$(MUSL_SRC_FILE):
	wget -c --no-check-certificate $(MUSL_SRC_URL) -O $(MUSL_SRC_FILE)

clean:
	$(RM) -r $(MUSL_INSTALL_DIR) $(MUSL_SRC_DIR)

clean-src:
	$(RM) $(MUSL_SRC_FILE)
