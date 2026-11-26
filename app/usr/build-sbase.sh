#!/bin/bash

set -e

if [[ -z $CC || -z $SBASE_SRC_DIR || -z $SBASE_SRC_FILE || -z $SBASE_INSTALL_DIR ]] ; then
    echo "CC, SBASE_SRC_DIR, SBASE_SRC_FILE and SBASE_INSTALL_DIR are not all set"
    exit 1
fi

rm -rf "$SBASE_SRC_DIR" "$SBASE_INSTALL_DIR"
unzip -q "$SBASE_SRC_FILE"
pushd "$SBASE_SRC_DIR"

patch -p1 < ../sbase.patch
make PREFIX="$SBASE_INSTALL_DIR" AR=ar RANLIB=ranlib \
    CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer" -j 1 sbase-box-install

popd
