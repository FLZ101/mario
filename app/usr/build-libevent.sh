#!/bin/bash

set -e

if [[ -z $CC || -z $LIBEVENT_SRC_DIR || -z $LIBEVENT_INSTALL_DIR ]] ; then
    echo "CC, LIBEVENT_SRC_DIR and LIBEVENT_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $LIBEVENT_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $LIBEVENT_SRC_DIR

export CC
export CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer"

./configure --host=i686-pc-linux-gnu --prefix="$LIBEVENT_INSTALL_DIR" --disable-shared --enable-static \
  --disable-samples --disable-libevent-regress --disable-thread-support \
  --disable-openssl --disable-debug-mode

make -j "${JOBS:-1}"
make install STRIP=true
popd
