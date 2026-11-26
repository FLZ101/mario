#!/bin/bash

set -e

if [[ -z $CC || -z $WGET_SRC_DIR || -z $WGET_INSTALL_DIR || -z $OPENSSL_INSTALL_DIR ]] ; then
    echo "CC, WGET_SRC_DIR, WGET_INSTALL_DIR and OPENSSL_INSTALL_DIR are not all set"
    exit 1
fi

source_dir=$(realpath "$WGET_SRC_DIR")
build_dir="$source_dir/mario"
rm -rf "$build_dir" "$WGET_INSTALL_DIR"
mkdir "$build_dir"
pushd "$build_dir"

PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$OPENSSL_INSTALL_DIR/lib/pkgconfig" \
    "$source_dir/configure" \
    --build="$(sh "$source_dir/build-aux/config.guess")" --host=i686-linux-musl \
    --prefix="$WGET_INSTALL_DIR" --with-ssl=openssl \
    --without-libpsl --without-zlib --disable-iri --disable-ipv6 --disable-nls \
    --disable-pcre --disable-pcre2 --disable-ntlm --disable-opie --disable-xattr \
    CC="$CC" AR=ar RANLIB=ranlib \
    CFLAGS="-ggdb ${OPTIMIZE:--O0} -fno-omit-frame-pointer"

make -j "${JOBS:-1}"
make -C src install-binPROGRAMS

popd
