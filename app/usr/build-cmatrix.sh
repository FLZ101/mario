#!/bin/bash

set -e

if [[ -z $CC || -z $CMATRIX_SRC_DIR || -z $CMATRIX_INSTALL_DIR ]] ; then
    echo "CC, CMATRIX_SRC_DIR and CMATRIX_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $CMATRIX_INSTALL_DIR/cmatrix ]] ; then
    exit 0
fi
mkdir -p $CMATRIX_INSTALL_DIR

pushd $CMATRIX_SRC_DIR

CPPFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"
LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib -static"
LIBS="-lncurses"

# source files must be before libs
$CC -o $CMATRIX_INSTALL_DIR/cmatrix *.c $CPPFLAGS $CFLAGS $LDFLAGS $LIBS

popd
