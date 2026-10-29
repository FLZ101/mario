#!/bin/bash
set -e
: "${CC:?}" "${LIBEVENT_SRC_DIR:?}" "${LIBEVENT_INSTALL_DIR:?}"
[[ -e $LIBEVENT_INSTALL_DIR ]] && exit 0
pushd "$LIBEVENT_SRC_DIR"
export CC
export CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer"
./configure --host=i686-pc-linux-gnu --prefix="$LIBEVENT_INSTALL_DIR" --disable-shared --enable-static \
  --disable-samples --disable-libevent-regress --disable-thread-support \
  --disable-openssl --disable-debug-mode
make -j1
make install STRIP=true
popd
