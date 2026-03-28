#!/bin/bash

set -e

if [[ -z $CC || -z $MUSL_SRC_DIR || -z $MUSL_INSTALL_DIR ]] ; then
    echo "CC, MUSL_SRC_DIR and MUSL_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $MUSL_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $MUSL_SRC_DIR

make distclean

./configure \
    --prefix="$MUSL_INSTALL_DIR" \
    --target=i386 \
    --enable-debug \
    --disable-shared \
    CC="$CC" \
    CFLAGS="-std=gnu11 -ggdb -O0 -fno-omit-frame-pointer" \
    AR=ar \
    RANLIB=ranlib

make -j 1
make install

popd
