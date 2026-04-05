#!/bin/bash

set -e

if [[ -z $CC || -z $NCURSES_SRC_DIR || -z $NCURSES_INSTALL_DIR ]] ; then
    echo "CC, NCURSES_SRC_DIR and NCURSES_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $NCURSES_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $NCURSES_SRC_DIR

if [[ ! -e Makefile ]] ; then
    ./configure \
        --prefix="$NCURSES_INSTALL_DIR" \
        --without-ada \
        --without-cxx \
        --without-cxx-binding \
        --without-shared \
        --enable-sigwinch \
        --without-pthread \
        --disable-reentrant \
        --without-develop \
        CC="$CC" \
        CFLAGS="-ggdb -O0 -fno-omit-frame-pointer -ffunction-sections" \
        AR=ar \
        RANLIB=ranlib
else
    make distclean
fi

make -j 1
make install

pushd $NCURSES_INSTALL_DIR/lib

rm -f libncurses.a && ln -s libncursesw.a libncurses.a
rm -f libcurses.a && ln -s libncursesw.a libcurses.a

popd

popd
