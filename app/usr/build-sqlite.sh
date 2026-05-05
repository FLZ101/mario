#!/bin/bash

set -e

if [[ -z $CC || -z $SQLITE_SRC_DIR || -z $SQLITE_INSTALL_DIR ]] ; then
    echo "CC, SQLITE_SRC_DIR and SQLITE_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -e $SQLITE_INSTALL_DIR ]] ; then
    exit 0
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -z $READLINE_INSTALL_DIR ]] ; then
    echo "READLINE_INSTALL_DIR is not set"
    exit 1
fi

pushd $SQLITE_SRC_DIR

export CC

export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"

./configure \
    --prefix="$SQLITE_INSTALL_DIR" \
    --disable-shared \
    --disable-threadsafe \
    --with-tempstore=no \
    --static-cli-shell \
    --debug \
    --with-readline-cflags="-I${READLINE_INSTALL_DIR}/include/ -I${READLINE_INSTALL_DIR}/include/readline -I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw" \
    --with-readline-ldflags="-L${READLINE_INSTALL_DIR}/lib -L${NCURSES_INSTALL_DIR}/lib -lreadline -lncurses"

make -j 1

# Keep debug info
make install STRIP=true

popd
