#!/bin/bash

set -e

if [[ -z $CC || -z $ZLIB_SRC_DIR || -z $ZLIB_INSTALL_DIR ]] ; then
    echo "CC, ZLIB_SRC_DIR and ZLIB_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $ZLIB_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $ZLIB_SRC_DIR

export CC

export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"

./configure \
    --prefix="$ZLIB_INSTALL_DIR"

make -j 1

# Keep debug info
make install STRIP=true

popd
