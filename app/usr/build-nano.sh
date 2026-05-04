#!/bin/bash

set -e

if [[ -z $CC || -z $NANO_SRC_DIR || -z $NANO_INSTALL_DIR ]] ; then
    echo "CC, NANO_SRC_DIR and NANO_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $NANO_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $NANO_SRC_DIR

export CC
export CPPFLAGS="-U__linux__ -I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"
export LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib"
export LIBS="-lncurses"

# export NCURSESW_CFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
# export NCURSESW_LIBS="-L${NCURSES_INSTALL_DIR}/lib -lncursesw_g"
# export NCURSES_CFLAGS="$NCURSESW_CFLAGS"
# export NCURSES_LIBS="$NCURSESW_LIBS"

export NCURSES_LIBS="-lncurses"

./configure \
    --host=i386-mario-elf \
    --prefix="$NANO_INSTALL_DIR" \
    --sysconfdir=/etc \
    --disable-threads \
    --disable-nls \
    --disable-speller \
    --enable-utf8=no \
    --enable-year2038

make -j 1

# Keep debug info
make install STRIP=true

popd
