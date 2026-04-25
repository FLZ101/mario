#!/bin/bash

set -e

if [[ -z $CC || -z $TOYBOX_SRC_DIR || -z $TOYBOX_INSTALL_DIR ]] ; then
    echo "CC, TOYBOX_SRC_DIR and TOYBOX_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $TOYBOX_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $TOYBOX_SRC_DIR

export CC
export LDFLAGS=""
export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer -D__linux__ -Wno-maybe-uninitialized"
export CROSS_COMPILE=

# make menuconfig
make defconfig
cp ../toybox/.config .
make toybox -j1 # V=1
PREFIX=$TOYBOX_INSTALL_DIR/bin make install_flat

popd
