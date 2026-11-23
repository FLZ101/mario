MEDIA_DIR := $(ROOT)/video
MEDIA_BUILD_DIR := $(MEDIA_DIR)/build
MEDIA_INSTALL_DIR := $(MEDIA_DIR)/install
MPV_PYTHON ?= $(abspath $(ROOT)/../../.venv/bin/python)

FFMPEG_SRC := FFmpeg-n7.1.1
FFMPEG_URL := https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1.1
FFMPEG_SHA256 := f117507dc501f2a6c11f9241d8d0c3213846cfad91764361af37befd6b6c523d
FFMPEG_PATCH := $(MEDIA_DIR)/ffmpeg.patch

PLACEBO_SRC := libplacebo-6.338.2
PLACEBO_URL := https://codeload.github.com/haasn/libplacebo/tar.gz/refs/tags/v6.338.2
PLACEBO_SHA256 := 2f1e624e09d72a8c9db70f910f7560e764a1c126dae42acc5b3bcef836a7aec6
PLACEBO_PATCH := $(MEDIA_DIR)/libplacebo.patch
PLACEBO_OVERLAYS := placebo-convert.c:src/convert.c

MPV_SRC := mpv-0.41.0
MPV_URL := https://codeload.github.com/mpv-player/mpv/tar.gz/refs/tags/v0.41.0
MPV_SHA256 := ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
MPV_PATCH := $(MEDIA_DIR)/mpv.patch
MPV_OVERLAYS := mpv-osd-null.c:sub/osd_null.c mpv-ao-mario.c:audio/out/ao_mario.c \
	mpv-vo-mario.c:video/out/vo_mario.c mpv-main-mario.c:osdep/main-fn-mario.c

LIBASS_SRC := libass-0.17.5
LIBASS_URL := https://github.com/libass/libass/releases/download/0.17.5/libass-0.17.5.tar.gz
LIBASS_SHA256 := caab4b993dd7be6187c55623b789ed75dddefea6e65938af134637c732fe094a

HARFBUZZ_SRC := harfbuzz-15.2.0
HARFBUZZ_URL := https://github.com/harfbuzz/harfbuzz/releases/download/15.2.0/harfbuzz-15.2.0.tar.xz
HARFBUZZ_SHA256 := e2ec49090744383cdbe7df808c1cedaafba06a97be8dc4211fe546ba585f12cb
HARFBUZZ_ARCHIVE_EXT := tar.xz

FRIBIDI_SRC := fribidi-1.0.18
FRIBIDI_URL := https://github.com/fribidi/fribidi/releases/download/v1.0.18/fribidi-1.0.18.tar.xz
FRIBIDI_SHA256 := 1de5e8073d035388b71d0b2d452b1da1f8caeea8a7702997b401fe1b6250727f
FRIBIDI_ARCHIVE_EXT := tar.xz

PCM_UAPI_DIR := $(abspath $(ROOT)/../../src/include/uapi)

overlay_source = $(MEDIA_DIR)/$(word 1,$(subst :, ,$(1)))
overlay_dest = $(word 2,$(subst :, ,$(1)))

define media_source
$(1)_SRC_DIR := $(MEDIA_DIR)/$($(1)_SRC)
$(1)_ARCHIVE_EXT ?= tar.gz
$(1)_ARCHIVE := $(MEDIA_DIR)/$($(1)_SRC).$$($(1)_ARCHIVE_EXT)

$$($(1)_ARCHIVE):
	wget --tries=3 '$($(1)_URL)' -O "$$@.tmp"
	printf '%s  %s\n' '$($(1)_SHA256)' "$$@.tmp" | sha256sum -c -
	mv "$$@.tmp" "$$@"

$$($(1)_SRC_DIR)/.prepared: $$($(1)_ARCHIVE) $($(1)_PATCH) $(foreach overlay,$($(1)_OVERLAYS),$(call overlay_source,$(overlay))) $(MEDIA_DIR)/mpv.mk
	printf '%s  %s\n' '$($(1)_SHA256)' "$$<" | sha256sum -c -
	$(RM) -r "$$($(1)_SRC_DIR)"
	tar xf "$$<" -C "$(MEDIA_DIR)"
	$(if $($(1)_PATCH),patch -d "$$($(1)_SRC_DIR)" -p1 < "$($(1)_PATCH)")
	$(foreach overlay,$($(1)_OVERLAYS),cp "$(call overlay_source,$(overlay))" "$$($(1)_SRC_DIR)/$(call overlay_dest,$(overlay))" &&) true
	touch "$$@"
endef

$(foreach project,FFMPEG PLACEBO MPV LIBASS HARFBUZZ FRIBIDI,$(eval $(call media_source,$(project))))

MEDIA_SOURCES := $(FFMPEG_SRC_DIR)/.prepared $(PLACEBO_SRC_DIR)/.prepared \
	$(MPV_SRC_DIR)/.prepared $(LIBASS_SRC_DIR)/.prepared \
	$(HARFBUZZ_SRC_DIR)/.prepared $(FRIBIDI_SRC_DIR)/.prepared
MEDIA_RECIPES := $(MEDIA_DIR)/build-ffmpeg.sh $(MEDIA_DIR)/build-mpv.sh
MEDIA_ENV = CC="$(CC)" MEDIA_BUILD_DIR="$(MEDIA_BUILD_DIR)" MEDIA_INSTALL_DIR="$(MEDIA_INSTALL_DIR)"

.PHONY:: build-mpv src-mpv clean-mpv clean-src-mpv
build:: build-mpv
build-mpv: $(MEDIA_INSTALL_DIR)/bin/mpv $(MEDIA_INSTALL_DIR)/bin/mario-mpv

src-mpv: $(MEDIA_SOURCES)

$(MEDIA_INSTALL_DIR)/bin/mpv $(MEDIA_INSTALL_DIR)/bin/mario-mpv &: $(MEDIA_SOURCES) $(MEDIA_RECIPES) $(CC) $(PCM_UAPI_DIR)/mario/pcm.h \
		$(MARIO_UI_LIB) $(ROOT)/../include/app/gui-limits.h \
		$(ROOT)/../../musl/musl-1.2.5-install/lib/libc.a $(ROOT)/../../compiler-rt/libcompiler_rt.a
	$(RM) -r "$(MEDIA_BUILD_DIR)" "$(MEDIA_INSTALL_DIR)"
	$(MEDIA_ENV) FFMPEG_SRC_DIR="$(FFMPEG_SRC_DIR)" bash "$(MEDIA_DIR)/build-ffmpeg.sh"
	$(MEDIA_ENV) PYTHON="$(MPV_PYTHON)" PCM_UAPI_DIR="$(PCM_UAPI_DIR)" PLACEBO_SRC_DIR="$(PLACEBO_SRC_DIR)" \
		MARIO_UI_DIR="$(MARIO_UI_DIR)" MPV_SRC_DIR="$(MPV_SRC_DIR)" bash "$(MEDIA_DIR)/build-mpv.sh"

clean:: clean-mpv
clean-mpv:
	$(RM) -r "$(MEDIA_BUILD_DIR)" "$(MEDIA_INSTALL_DIR)" "$(FFMPEG_SRC_DIR)" \
		"$(PLACEBO_SRC_DIR)" "$(MPV_SRC_DIR)" "$(LIBASS_SRC_DIR)" \
		"$(HARFBUZZ_SRC_DIR)" "$(FRIBIDI_SRC_DIR)"

clean-src:: clean-src-mpv
clean-src-mpv:
	$(RM) $(foreach archive,$(FFMPEG_ARCHIVE) $(PLACEBO_ARCHIVE) $(MPV_ARCHIVE) $(LIBASS_ARCHIVE) $(HARFBUZZ_ARCHIVE) $(FRIBIDI_ARCHIVE),"$(archive)" "$(archive).tmp")
