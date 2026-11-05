FREETYPE_VER		:=2.13.3
FREETYPE_SRC_DIR	:=freetype-$(FREETYPE_VER)
FREETYPE_SRC_FILE	:=$(FREETYPE_SRC_DIR).tar.gz
FREETYPE_SRC_URL	:=https://download.savannah.gnu.org/releases/freetype/$(FREETYPE_SRC_FILE)
FREETYPE_SRC_SHA256	:=5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747

# FreeType is dual-licensed under the FreeType License or GPL-2.0-only.
# The verified archive carries both texts in docs/FTL.TXT and docs/GPLv2.TXT.

.PHONY build:: src-freetype

src-freetype: $(FREETYPE_SRC_DIR)/include/freetype/freetype.h

$(FREETYPE_SRC_DIR)/include/freetype/freetype.h: | $(FREETYPE_SRC_FILE)
	@printf '%s  %s\n' '$(FREETYPE_SRC_SHA256)' '$(FREETYPE_SRC_FILE)' | sha256sum -c -
	tar --no-same-owner -xf $(FREETYPE_SRC_FILE)

$(FREETYPE_SRC_FILE):
	wget -c --no-check-certificate $(FREETYPE_SRC_URL) -O $(FREETYPE_SRC_FILE)
