#!/bin/bash

set -e

if [[ -z $CC || -z $FFMPEG_SRC_DIR || -z $MEDIA_BUILD_DIR || -z $MEDIA_INSTALL_DIR ]] ; then
    echo "CC, FFMPEG_SRC_DIR, MEDIA_BUILD_DIR and MEDIA_INSTALL_DIR are not all set"
    exit 1
fi

unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
export PKG_CONFIG_LIBDIR="$MEDIA_INSTALL_DIR/lib/pkgconfig"
export GIT_CEILING_DIRECTORIES="$(dirname "$FFMPEG_SRC_DIR")"

mkdir -p "$MEDIA_BUILD_DIR/ffmpeg"
pushd "$MEDIA_BUILD_DIR/ffmpeg"

"$FFMPEG_SRC_DIR/configure" \
    --prefix="$MEDIA_INSTALL_DIR" \
    --cc="$CC" --ld="$CC" --ar=ar --ranlib=ranlib \
    --enable-cross-compile --arch=i386 --cpu=i686 --target-os=linux \
    --disable-everything --disable-autodetect --disable-network \
    --disable-programs --disable-doc --disable-debug --disable-asm --disable-avdevice \
    --disable-shared --enable-static \
    --enable-protocol=file --enable-demuxer=mov \
    --enable-decoder=aac --enable-parser=aac \
    --enable-filter=abuffer,abuffersink,aformat,aresample,anull \
    --extra-cflags="-idirafter $(gcc -print-file-name=include) -mno-mmx -mno-sse -mno-sse2 -ffunction-sections -fdata-sections"

make -j "${JOBS:-1}"
make install

popd
