#!/bin/bash

set -e

if [[ -z $CC || -z $READLINE_SRC_DIR || -z $READLINE_INSTALL_DIR ]] ; then
    echo "CC, READLINE_SRC_DIR and READLINE_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $READLINE_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $READLINE_SRC_DIR

export CC

export CPPFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"
export LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib"

./configure \
    --prefix="$READLINE_INSTALL_DIR" \
    --enable-multibyte=no \
    --enable-shared=no \
    --enable-year2038 \
    --with-curses

make -j 1

# Keep debug info
make install STRIP=true

popd
