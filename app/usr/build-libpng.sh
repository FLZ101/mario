#!/bin/bash

set -e

if [[ -z $CC || -z $LIBPNG_SRC_DIR || -z $LIBPNG_INSTALL_DIR || -z $ZLIB_INSTALL_DIR ]] ; then
    echo "CC, LIBPNG_SRC_DIR, LIBPNG_INSTALL_DIR, and ZLIB_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $LIBPNG_INSTALL_DIR ]] ; then
    exit 0
fi

pushd "$LIBPNG_SRC_DIR"

export CC
export CPPFLAGS="-I$ZLIB_INSTALL_DIR/include"
export LDFLAGS="-L$ZLIB_INSTALL_DIR/lib"
export CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer"

./configure \
    --host=i686-pc-linux-gnu \
    --prefix="$LIBPNG_INSTALL_DIR" \
    --disable-shared \
    --enable-static \
    --disable-tools \
    --disable-tests

make -j "${JOBS:-1}"

# Keep debug info in the static decoder archive for guest backtraces.
make install STRIP=true

popd
