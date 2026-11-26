#!/bin/bash

set -e

if [[ -z $CC || -z $OPENSSH_SRC_DIR || -z $OPENSSH_INSTALL_DIR || -z $OPENSSL_INSTALL_DIR || -z $ZLIB_INSTALL_DIR ]] ; then
    echo "CC, OPENSSH_SRC_DIR, OPENSSH_INSTALL_DIR, OPENSSL_INSTALL_DIR and ZLIB_INSTALL_DIR are not all set"
    exit 1
fi

source_dir=$(realpath "$OPENSSH_SRC_DIR")
build_dir="$source_dir/mario"
rm -rf "$build_dir" "$OPENSSH_INSTALL_DIR"
mkdir "$build_dir"
pushd "$build_dir"

"$source_dir/configure" \
    --build="$(sh "$source_dir/config.guess")" --host=i686-linux-musl \
    --prefix=/usr --sysconfdir=/etc/ssh \
    --with-ssl-dir="$OPENSSL_INSTALL_DIR" --with-zlib="$ZLIB_INSTALL_DIR" \
    --disable-pkcs11 --disable-security-key --without-pam --without-libedit \
    CC="$CC" AR=ar RANLIB=ranlib \
    CFLAGS="-ggdb ${OPTIMIZE:--O0} -fno-omit-frame-pointer"

make -j "${JOBS:-1}" ssh ssh-keygen
install -Dm755 -t "$OPENSSH_INSTALL_DIR/bin" ssh ssh-keygen

popd
