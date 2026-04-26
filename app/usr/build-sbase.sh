#!/bin/bash

set -e

if [[ -z $CC || -z $SBASE_SRC_DIR || -z $SBASE_INSTALL_DIR ]] ; then
    echo "CC, SBASE_SRC_DIR and SBASE_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $SBASE_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $SBASE_SRC_DIR

make PREFIX=$SBASE_INSTALL_DIR AR=ar RANLIB=ranlib \
    CFLAGS="-ggdb -O0 -fno-omit-frame-pointer" -j 1 sbase-box-install

popd
