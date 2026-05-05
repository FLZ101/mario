#!/bin/bash

set -e

if [[ -z $CC || -z $PYTHON_SRC_DIR || -z $PYTHON_INSTALL_DIR ]] ; then
    echo "CC, PYTHON_SRC_DIR and PYTHON_INSTALL_DIR are not all set"
    exit 1
fi

if [[ -z $NCURSES_INSTALL_DIR ]] ; then
    echo "NCURSES_INSTALL_DIR is not set"
    exit 1
fi

if [[ -z $READLINE_INSTALL_DIR ]] ; then
    echo "READLINE_INSTALL_DIR is not set"
    exit 1
fi

if [[ -z $SQLITE_INSTALL_DIR ]] ; then
    echo "SQLITE_INSTALL_DIR is not set"
    exit 1
fi

if [[ -z $ZLIB_INSTALL_DIR ]] ; then
    echo "ZLIB_INSTALL_DIR is not set"
    exit 1
fi

pushd $PYTHON_SRC_DIR

build_for_linux()
{
    PYTHON_INSTALL_DIR_LINUX="$PYTHON_INSTALL_DIR-linux"

    if [[ -e $PYTHON_INSTALL_DIR_LINUX ]] ; then
        return 0
    fi

    rm -f Modules/Setup.local

    mkdir -p linux
    pushd linux

    printenv | grep musl

    CC="" LD="" ../configure \
        --prefix="$PYTHON_INSTALL_DIR_LINUX"

    CC="" LD="" MAKEFLAGS="" make -j 1
    CC="" LD="" MAKEFLAGS="" make install

    popd
}

build_for_linux

build_for_mario()
{
    if [[ -e $PYTHON_INSTALL_DIR ]] ; then
        return 0
    fi

    mkdir -p mario
    pushd mario

    mkdir Modules
    cp "../../python.Setup.local" Modules/Setup.local

    export CC
    export LD
    export CFLAGS="-ggdb -O0 -fno-omit-frame-pointer"

    export CURSES_CFLAGS="-I${NCURSES_INSTALL_DIR}/include -I${NCURSES_INSTALL_DIR}/include/ncursesw"
    export CURSES_LIBS="-L${NCURSES_INSTALL_DIR}/lib -lncurses -lpanel"

    export LIBREADLINE_CFLAGS="-I${READLINE_INSTALL_DIR}/include -I${READLINE_INSTALL_DIR}/include/readline"
    export LIBREADLINE_LIBS="-L${READLINE_INSTALL_DIR}/lib -lreadline"

    export ZLIB_CFLAGS="-I${ZLIB_INSTALL_DIR}/include"
    export ZLIB_LIBS="-L${ZLIB_INSTALL_DIR}/lib -lz"

    export LIBSQLITE3_CFLAGS="-I${SQLITE_INSTALL_DIR}/include"
    export LIBSQLITE3_LIBS="-L${SQLITE_INSTALL_DIR}/lib -lsqlite3"

    ../configure \
        --prefix="$PYTHON_INSTALL_DIR" \
        --build=x86_64 \
        --host=i386-linux \
        --with-build-python="$PYTHON_INSTALL_DIR_LINUX/bin/python3" \
        --enable-ipv6=no \
        --disable-shared \
        --disable-test-modules \
        --with-pkg-config=no \
        --with-c-locale-coercion=no \
        --with-ensurepip=no \
        ac_cv_file__dev_ptmx=no \
        ac_cv_file__dev_ptc=no \
        ac_cv_func_dlopen=no \
        ac_cv_lib_dl_dlopen=no \
        py_cv_module__socket=no

    # config.log

    make LINKFORSHARED=" " -j 1

    # Keep debug info
    make install STRIP=true

    popd

    PYTHON_INSTALL_DIR_MARIO="$PYTHON_INSTALL_DIR-mario"
    rm -rf "$PYTHON_INSTALL_DIR_MARIO"
    cp -r "$PYTHON_INSTALL_DIR" "$PYTHON_INSTALL_DIR_MARIO"
    pushd "$PYTHON_INSTALL_DIR_MARIO/lib/python3.13"

    for x in $(find . -name __pycache__) ; do
        echo "$x"
        rm -r "$x"
    done

    rm -r config-3.13-i386-linux-musl/ \
        email/ \
        ensurepip/ \
        http/ \
        idlelib/ \
        tkinter/ \
        turtledemo/ \
        venv/ \
        wsgiref/ \
        ctypes/

    rm encodings/cp*.py encodings/iso*.py encodings/mac*.py

    # _sysconfigdata__linux_i386-linux-musl.py is too long for MarioFS
    mv _sysconfigdata__linux_i386-linux-musl.py _sysconfigdata_mario.py
    cat <<EOF >>sysconfig/__init__.py


def _get_sysconfigdata_name():
    return os.environ.get(
        '_PYTHON_SYSCONFIGDATA_NAME',
        '_sysconfigdata_mario',
    )
EOF

    popd
}

build_for_mario
