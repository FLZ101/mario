#!/bin/bash

set -e

if [[ -z $CC || -z $LIBEDIT_SRC_DIR || -z $LIBEDIT_INSTALL_DIR ]] ; then
    echo "CC, LIBEDIT_SRC_DIR and LIBEDIT_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $LIBEDIT_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $LIBEDIT_SRC_DIR

if [[ ! -e Makefile ]] ; then
    ./configure \
        --prefix="$LIBEDIT_INSTALL_DIR" \
        --enable-static \
        --disable-shared \
        --disable-pic \
        CC="$CC" \
        CFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw -ggdb -O0 -fno-omit-frame-pointer -ffunction-sections -D__STDC_ISO_10646__" \
        LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib" \
        LIBS="-lncurses" \
        AR=ar \
        RANLIB=ranlib
else
    make distclean
fi

make -j 1
make install

popd
