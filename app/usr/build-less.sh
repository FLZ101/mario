#!/bin/bash

set -e

if [[ -z $CC || -z $LESS_SRC_DIR || -z $LESS_INSTALL_DIR ]] ; then
    echo "CC, LESS_SRC_DIR and LESS_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $LESS_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $LESS_SRC_DIR

export CC
export CPPFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"
export LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib"
export LIBS="-lncurses"

./configure \
    --prefix="$LESS_INSTALL_DIR" \
    --sysconfdir=/etc \
    --enable-year2038 \
    --with-editor=vim

make -j 1

# Keep debug info
make install STRIP=true

popd
