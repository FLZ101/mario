MEDIA_DIR := $(ROOT)/video
MEDIA_BUILD_DIR := $(MEDIA_DIR)/build
MEDIA_INSTALL_DIR := $(MEDIA_DIR)/install
MPV_PYTHON ?= $(abspath $(ROOT)/../../.venv/bin/python)

FFMPEG_SRC := FFmpeg-n7.1.1
FFMPEG_URL := https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1.1
FFMPEG_SHA256 := f117507dc501f2a6c11f9241d8d0c3213846cfad91764361af37befd6b6c523d

PLACEBO_SRC := libplacebo-6.338.2
PLACEBO_URL := https://codeload.github.com/haasn/libplacebo/tar.gz/refs/tags/v6.338.2
PLACEBO_SHA256 := 2f1e624e09d72a8c9db70f910f7560e764a1c126dae42acc5b3bcef836a7aec6
PLACEBO_PATCH := $(MEDIA_DIR)/libplacebo.patch
PLACEBO_OVERLAY := $(MEDIA_DIR)/placebo-convert.c
PLACEBO_DEST := src/convert.c

MPV_SRC := mpv-0.41.0
MPV_URL := https://codeload.github.com/mpv-player/mpv/tar.gz/refs/tags/v0.41.0
MPV_SHA256 := ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
MPV_PATCH := $(MEDIA_DIR)/mpv.patch
MPV_OVERLAY := $(MEDIA_DIR)/mpv-osd-null.c
MPV_DEST := sub/osd_null.c

define media_source
$(1)_SRC_DIR := $(MEDIA_DIR)/$($(1)_SRC)
$(1)_ARCHIVE := $(MEDIA_DIR)/$($(1)_SRC).tar.gz

$$($(1)_ARCHIVE):
	wget --tries=3 '$($(1)_URL)' -O "$$@.tmp"
	printf '%s  %s\n' '$($(1)_SHA256)' "$$@.tmp" | sha256sum -c -
	mv "$$@.tmp" "$$@"

$$($(1)_SRC_DIR)/.prepared: $$($(1)_ARCHIVE) $($(1)_PATCH) $($(1)_OVERLAY) $(MEDIA_DIR)/mpv.mk
	printf '%s  %s\n' '$($(1)_SHA256)' "$$<" | sha256sum -c -
	$(RM) -r "$$($(1)_SRC_DIR)"
	tar xf "$$<" -C "$(MEDIA_DIR)"
	$(if $($(1)_PATCH),patch -d "$$($(1)_SRC_DIR)" -p1 < "$($(1)_PATCH)")
	$(if $($(1)_OVERLAY),cp "$($(1)_OVERLAY)" "$$($(1)_SRC_DIR)/$($(1)_DEST)")
	touch "$$@"
endef

$(foreach project,FFMPEG PLACEBO MPV,$(eval $(call media_source,$(project))))

MEDIA_SOURCES := $(FFMPEG_SRC_DIR)/.prepared $(PLACEBO_SRC_DIR)/.prepared $(MPV_SRC_DIR)/.prepared
MEDIA_RECIPES := $(MEDIA_DIR)/build-ffmpeg.sh $(MEDIA_DIR)/build-mpv.sh
MEDIA_ENV = CC="$(CC)" MEDIA_BUILD_DIR="$(MEDIA_BUILD_DIR)" MEDIA_INSTALL_DIR="$(MEDIA_INSTALL_DIR)"

.PHONY:: build-mpv src-mpv clean-mpv clean-src-mpv
build:: build-mpv
build-mpv: $(MEDIA_INSTALL_DIR)/bin/mpv

src-mpv: $(MEDIA_SOURCES)

$(MEDIA_INSTALL_DIR)/bin/mpv: $(MEDIA_SOURCES) $(MEDIA_RECIPES) $(CC) \
		$(ROOT)/../../musl/musl-1.2.5-install/lib/libc.a $(ROOT)/../../compiler-rt/libcompiler_rt.a
	$(RM) -r "$(MEDIA_BUILD_DIR)" "$(MEDIA_INSTALL_DIR)"
	$(MEDIA_ENV) FFMPEG_SRC_DIR="$(FFMPEG_SRC_DIR)" bash "$(MEDIA_DIR)/build-ffmpeg.sh"
	$(MEDIA_ENV) PYTHON="$(MPV_PYTHON)" PLACEBO_SRC_DIR="$(PLACEBO_SRC_DIR)" \
		MPV_SRC_DIR="$(MPV_SRC_DIR)" bash "$(MEDIA_DIR)/build-mpv.sh"

clean:: clean-mpv
clean-mpv:
	$(RM) -r "$(MEDIA_BUILD_DIR)" "$(MEDIA_INSTALL_DIR)" "$(FFMPEG_SRC_DIR)" "$(PLACEBO_SRC_DIR)" "$(MPV_SRC_DIR)"

clean-src:: clean-src-mpv
clean-src-mpv:
	$(RM) $(foreach archive,$(FFMPEG_ARCHIVE) $(PLACEBO_ARCHIVE) $(MPV_ARCHIVE),"$(archive)" "$(archive).tmp")
