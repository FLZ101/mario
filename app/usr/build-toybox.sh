#!/bin/bash

set -e

if [[ -z $CC || -z $TOYBOX_SRC_DIR || -z $TOYBOX_SRC_FILE || -z $TOYBOX_INSTALL_DIR ]] ; then
    echo "CC, TOYBOX_SRC_DIR, TOYBOX_SRC_FILE and TOYBOX_INSTALL_DIR are not all set"
    exit 1
fi

rm -rf "$TOYBOX_SRC_DIR"
tar xf "$TOYBOX_SRC_FILE"
pushd "$TOYBOX_SRC_DIR"

export CC
export LDFLAGS=""
export CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer -D__linux__ -Wno-maybe-uninitialized"
export CROSS_COMPILE=

# make menuconfig
make defconfig
cp ../toybox/.config .
patch -p1 < ../toybox.patch
make toybox -j "${JOBS:-1}" # V=1
rm -rf "$TOYBOX_INSTALL_DIR"
PREFIX="$TOYBOX_INSTALL_DIR/bin" make install_flat

popd
