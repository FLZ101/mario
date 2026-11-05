#!/bin/bash

set -e

if [[ -z $CC || -z $TMUX_SRC_DIR || -z $TMUX_INSTALL_DIR ]] ; then
    echo "CC, TMUX_SRC_DIR and TMUX_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $LIBEVENT_INSTALL_DIR ]] ; then
    echo "LIBEVENT_INSTALL_DIR is not set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $TMUX_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $TMUX_SRC_DIR

export CC
export CPPFLAGS="-I${LIBEVENT_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
export CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer"
export LDFLAGS="-static -L${LIBEVENT_INSTALL_DIR}/lib -L${NCURSES_INSTALL_DIR}/lib"
export ac_cv_search_forkpty='none required'
export PKG_CONFIG=/bin/false

./configure --host=i686-pc-linux-gnu --prefix="$TMUX_INSTALL_DIR" --disable-utempter --disable-sixel

make -j "${JOBS:-1}"
make install STRIP=true
popd
