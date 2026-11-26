#!/bin/bash

set -e

if [[ -z $CC || -z $CURL_SRC_DIR || -z $CURL_INSTALL_DIR || -z $OPENSSL_INSTALL_DIR ]] ; then
    echo "CC, CURL_SRC_DIR, CURL_INSTALL_DIR and OPENSSL_INSTALL_DIR are not all set"
    exit 1
fi

source_dir=$(realpath "$CURL_SRC_DIR")
build_dir="$source_dir/mario"
rm -rf "$build_dir" "$CURL_INSTALL_DIR"
mkdir "$build_dir"
pushd "$build_dir"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$OPENSSL_INSTALL_DIR/lib/pkgconfig" \
    ac_cv_func_eventfd=no \
    "$source_dir/configure" \
    --build="$(sh "$source_dir/config.guess")" --host=i686-linux-musl \
    --prefix="$CURL_INSTALL_DIR" --disable-shared --enable-static \
    --with-openssl="$OPENSSL_INSTALL_DIR" --with-ca-bundle=/etc/ssl/cert.pem \
    --without-ca-path --without-ca-embed \
    --without-zlib --without-brotli --without-zstd --without-libpsl \
    --without-libidn2 --without-libgsasl --disable-ipv6 --disable-threaded-resolver \
    --disable-ftp --disable-file --disable-ldap --disable-ldaps --disable-rtsp \
    --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smtp --disable-gopher --disable-mqtt --disable-ipfs --disable-websockets \
    --disable-proxy --disable-unix-sockets --disable-docs --disable-manual \
    CC="$CC" AR=ar RANLIB=ranlib \
    CFLAGS="-ggdb ${OPTIMIZE:--O0} -fno-omit-frame-pointer"

make -j "${JOBS:-1}"
make -C src install-binPROGRAMS

popd
