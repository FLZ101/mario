#!/bin/bash

set -e

if [[ -z $CC || -z $UBASE_SRC_DIR || -z $UBASE_INSTALL_DIR ]] ; then
    echo "CC, UBASE_SRC_DIR and UBASE_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $UBASE_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $UBASE_SRC_DIR

make PREFIX=$UBASE_INSTALL_DIR AR=ar RANLIB=ranlib \
    CC=$CC \
    CFLAGS="-std=c99 -Wall -Wextra -ggdb -O0 -fno-omit-frame-pointer" -j 1 ubase-box-install

popd
