#!/bin/bash
set -e
: "${CC:?}" "${TMUX_SRC_DIR:?}" "${TMUX_INSTALL_DIR:?}" "${LIBEVENT_INSTALL_DIR:?}" "${NCURSES_INSTALL_DIR:?}"
[[ -d $LIBEVENT_INSTALL_DIR && -d $NCURSES_INSTALL_DIR ]]
[[ -e $TMUX_INSTALL_DIR ]] && exit 0
pushd "$TMUX_SRC_DIR"
export CC
export CPPFLAGS="-I${LIBEVENT_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
export CFLAGS="-ggdb ${OPTIMIZE} -fno-omit-frame-pointer"
export LDFLAGS="-static -L${LIBEVENT_INSTALL_DIR}/lib -L${NCURSES_INSTALL_DIR}/lib"
./configure --host=i686-pc-linux-gnu --prefix="$TMUX_INSTALL_DIR" --disable-utempter --disable-sixel
make -j1
make install STRIP=true
popd
