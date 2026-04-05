#!/bin/bash

set -e

if [[ -z $CC || -z $OKSH_SRC_DIR || -z $OKSH_INSTALL_DIR ]] ; then
    echo "CC, OKSH_SRC_DIR and OKSH_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $OKSH_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $OKSH_SRC_DIR

export LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib"
if [[ ! -e Makefile ]] ; then
    ./configure \
        --prefix="$OKSH_INSTALL_DIR" \
        --cc="$CC" \
        --cflags="-ggdb -O0 -fno-omit-frame-pointer -I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw" \
        --no-strip \
        --enable-curses \
        --enable-static \
        AR=ar \
        RANLIB=ranlib
else
    make distclean
fi

make -j 1
make install

popd
