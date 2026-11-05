#!/bin/bash

set -e

if [[ -z $CC || -z $FREETYPE_SRC_DIR || -z $FREETYPE_INSTALL_DIR ]] ; then
    echo "CC, FREETYPE_SRC_DIR and FREETYPE_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $FREETYPE_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $FREETYPE_SRC_DIR

source_dir=$(pwd)
build_dir="$FREETYPE_INSTALL_DIR/build"
mkdir -p "$build_dir"

# FreeType prefers modules.cfg from the out-of-tree object directory.  Keep
# only the SFNT/TrueType/CFF drivers and their required raster/PostScript code.
cat > "$build_dir/modules.cfg" <<'EOF'
FONT_MODULES += truetype
FONT_MODULES += cff
FONT_MODULES += sfnt
HINTING_MODULES += pshinter
RASTER_MODULES += smooth
AUX_MODULES += psaux
AUX_MODULES += psnames
EOF

pushd "$build_dir"

export CC
export CFLAGS="-c -ggdb ${OPTIMIZE} -fno-omit-frame-pointer"
# The Unix backend invokes autotools and probes host libraries.  Its ANSI
# backend builds a static archive directly, avoiding accidental host deps.
cp "$source_dir/builds/ansi/ansi.mk" config.mk
make -f "$source_dir/Makefile" \
    TOP_DIR="$source_dir" OBJ_DIR="$build_dir" CC="$CC" AR="ar -D" \
    CFLAGS="$CFLAGS" -j "${JOBS:-1}"
ranlib -D libfreetype.a

mkdir -p "$FREETYPE_INSTALL_DIR/lib" \
         "$FREETYPE_INSTALL_DIR/include/freetype/config"
cp libfreetype.a "$FREETYPE_INSTALL_DIR/lib/"
cp "$source_dir/include/ft2build.h" "$FREETYPE_INSTALL_DIR/include/"
cp -R "$source_dir/include/freetype/." "$FREETYPE_INSTALL_DIR/include/freetype/"
cp ftmodule.h "$FREETYPE_INSTALL_DIR/include/freetype/config/"

popd

popd
