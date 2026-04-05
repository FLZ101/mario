#!/bin/bash

set -e

if [[ -z $CC || -z $DASH_SRC_DIR || -z $DASH_INSTALL_DIR ]] ; then
    echo "CC, DASH_SRC_DIR and DASH_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $LIBEDIT_INSTALL_DIR ]] ; then
    echo "LIBEDIT_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $DASH_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $DASH_SRC_DIR

if [[ ! -e configure ]] ; then
    ./autogen.sh
fi

if [[ ! -e Makefile ]] ; then
    # --enable-fnmatch
    ./configure \
        --prefix="$DASH_INSTALL_DIR" \
        --enable-glob \
        --with-libedit \
        CC="$CC" \
        CFLAGS="-I${LIBEDIT_INSTALL_DIR}/include -I${LIBEDIT_INSTALL_DIR}/include/editline -I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw -ggdb -O0 -fno-omit-frame-pointer" \
        LDFLAGS="-L${LIBEDIT_INSTALL_DIR}/lib -L${NCURSES_INSTALL_DIR}/lib -static" \
        LIBS="-ledit -lncurses" \
        AR=ar \
        RANLIB=ranlib
else
    make distclean
fi

make -j 1
make install

popd
