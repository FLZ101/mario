#!/bin/bash

set -e

if [[ -z $CC || -z $VIM_SRC_DIR || -z $VIM_INSTALL_DIR ]] ; then
    echo "CC, VIM_SRC_DIR and VIM_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -e $VIM_INSTALL_DIR ]] ; then
    exit 0
fi

pushd $VIM_SRC_DIR

__undef()
{
    sed -i 's/# define '"$1"'//g' src/feature.h
}
# __undef FEAT_DIGRAPHS
# __undef FEAT_LANGMAP
# __undef FEAT_KEYMAP
# __undef FEAT_RIGHTLEFT
# __undef FEAT_ARABIC
# __undef FEAT_CSCOPE
__undef FEAT_RELTIME
# __undef FEAT_PRINTER
# __undef FEAT_SPELL
# __undef FEAT_SESSION
# __undef FEAT_MULTI_LANG
# __undef USE_ICONV
# __undef USE_DLOPEN
# __undef FEAT_SOUND
# __undef FEAT_SOUND_CANBERRA
# __undef FEAT_WRITEBACKUP
# __undef WANT_X11
# __undef WANT_WAYLAND
# __undef FEAT_MOUSE_XTERM
# __undef FEAT_MOUSE_NET
# __undef FEAT_MOUSE_DEC
# __undef FEAT_MOUSE_URXVT
# __undef FEAT_MOUSE_GPM
# __undef FEAT_SYSMOUSE
# __undef FEAT_SOCKETSERVER
# __undef FEAT_CLIENTSERVER
# __undef FEAT_TERMRESPONSE
# __undef CURSOR_SHAPE
# __undef FEAT_XATTR

export CC

export CPPFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"
export LDFLAGS="-L${NCURSES_INSTALL_DIR}/lib"

./configure \
    --prefix="$VIM_INSTALL_DIR" \
    --sysconfdir=/etc \
    --with-features=normal \
    --with-tlib=ncurses \
    --with-compiledby="mario" \
    --disable-darwin --disable-smack --disable-selinux --disable-xattr --disable-xsmp \
    --disable-xsmp-interact --enable-cscope=no --disable-netbeans --disable-channel \
    --enable-terminal=no --enable-autoservername=no --enable-socketserver=no \
    --enable-multibyte --disable-rightleft --disable-arabic \
    --enable-xim=no --enable-fontset=no --enable-gui=no \
    --disable-icon-cache-update --disable-desktop-database-update \
    --disable-canberra --disable-libsodium --disable-acl --enable-gpm=no \
    --disable-sysmouse --disable-nls --enable-year2038 \
    --with-wayland=no --with-x=no --with-gnome=no --disable-acl

make VIMRCLOC=/etc VIMRUNTIMEDIR=/usr/share/vim/vim92 -j 1

# Keep debug info
make install STRIP=true

popd
