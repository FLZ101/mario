#!/bin/bash

set -e

if [[ -z $CC || -z $OPENSSH_SRC_DIR || -z $OPENSSH_INSTALL_DIR || -z $OPENSSL_INSTALL_DIR || -z $ZLIB_INSTALL_DIR ]] ; then
    echo "CC, OPENSSH_SRC_DIR, OPENSSH_INSTALL_DIR, OPENSSL_INSTALL_DIR and ZLIB_INSTALL_DIR are not all set"
    exit 1
fi

source_dir=$(realpath "$OPENSSH_SRC_DIR")
build_dir="$source_dir/mario"
rm -rf "$build_dir" "$OPENSSH_INSTALL_DIR"
mkdir "$build_dir"
pushd "$build_dir"

# The app target invokes the recursive make with LD=$(APP_LD), i.e.
# util/mario-musl-ld, and LD is exported.  GNU make records command-line
# variable assignments in MAKEFLAGS, so the OpenSSH make launched below
# inherits LD=util/mario-musl-ld even though this script does not pass LD
# explicitly.  In that OpenSSH make the inherited value has command-line
# origin, so it overrides the LD=... assignment configure writes into the
# generated Makefile.  Record the musl compiler driver as the linker in
# configure, then pass the same value on the OpenSSH make command line below
# so the inherited mario-musl-ld value cannot win.  Without this, the direct
# ld wrapper receives compiler-driver flags such as -Wl,-z,relro and
# -fstack-protector-strong and fails.
# --without-pie keeps configure from appending -pie: mario-musl-cc emits
# non-PIC static objects, so PIE would not match Mario's static userland.
"$source_dir/configure" \
    --build="$(sh "$source_dir/config.guess")" --host=i686-linux-musl \
    --prefix=/usr --sysconfdir=/etc/ssh \
    --with-ssl-dir="$OPENSSL_INSTALL_DIR" --with-zlib="$ZLIB_INSTALL_DIR" \
    --disable-pkcs11 --disable-security-key --without-pam --without-libedit \
    --without-pie \
    CC="$CC" LD="$CC" AR=ar RANLIB=ranlib \
    CFLAGS="-ggdb ${OPTIMIZE:--O0} -fno-omit-frame-pointer"

# See the comment above: MAKEFLAGS still carries LD=util/mario-musl-ld into
# this recursive make.  Put LD=$CC on this command line so the OpenSSH
# Makefile actually links with the musl compiler driver instead of the
# direct ld wrapper.  The compiler driver understands -Wl,... and
# compiler-only flags; util/mario-musl-ld does not.
make LD="$CC" -j "${JOBS:-1}" ssh ssh-keygen scp sftp
install -Dm755 -t "$OPENSSH_INSTALL_DIR/bin" ssh ssh-keygen scp sftp

popd
