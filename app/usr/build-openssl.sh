#!/bin/bash

set -e

if [[ -z $CC || -z $OPENSSL_SRC_DIR || -z $OPENSSL_INSTALL_DIR ]] ; then
    echo "CC, OPENSSL_SRC_DIR and OPENSSL_INSTALL_DIR are not all set"
    exit 1
fi

source_dir=$(realpath "$OPENSSL_SRC_DIR")
build_dir="$source_dir/mario"
rm -rf "$build_dir" "$OPENSSL_INSTALL_DIR"
mkdir "$build_dir"
pushd "$build_dir"

CC="$CC" AR=ar RANLIB=ranlib "$source_dir/Configure" linux-generic32 \
    --prefix="$OPENSSL_INSTALL_DIR" --libdir=lib --openssldir=/etc/ssl \
    --with-rand-seed=os \
    no-asm no-apps no-dso no-module no-shared no-tests \
    no-afalgeng no-async no-ktls no-quic no-secure-memory \
    -D__STDC_NO_ATOMICS__=1 \
    -ggdb "${OPTIMIZE:--O0}" -fno-omit-frame-pointer

make -j "${JOBS:-1}" build_libs
make install_dev
touch "$OPENSSL_INSTALL_DIR/.mario-build"

popd
