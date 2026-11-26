#!/bin/bash

set -e

if [[ -z $CC || -z $NET_TOOLS_SRC_DIR || -z $NET_TOOLS_SRC_FILE || -z $NET_TOOLS_INSTALL_DIR ]] ; then
    echo "CC, NET_TOOLS_SRC_DIR, NET_TOOLS_SRC_FILE and NET_TOOLS_INSTALL_DIR are not all set"
    exit 1
fi

rm -rf "$NET_TOOLS_SRC_DIR" "$NET_TOOLS_INSTALL_DIR"
tar xf "$NET_TOOLS_SRC_FILE"
patch --fuzz=0 -d "$NET_TOOLS_SRC_DIR" -p1 < nettools.patch
mkdir -p "$NET_TOOLS_INSTALL_DIR/bin"

pushd "$NET_TOOLS_SRC_DIR"
printf '#define HAVE_AFINET 1\n#define HAVE_HWETHER 1\n' > config.h
make version.h

"$CC" -ggdb "${OPTIMIZE:--O0}" -fno-omit-frame-pointer -DMARIO_NET_TOOLS \
    -I. -Iinclude -Ilib -I../../include -o "$NET_TOOLS_INSTALL_DIR/bin/ifconfig" \
    ../nettools.c lib/interface.c lib/inet_gr.c lib/inet.c lib/af.c lib/hw.c \
    lib/ether.c lib/loopback.c lib/unix.c lib/util.c

popd

for command in route netstat; do
    ln -s ifconfig "$NET_TOOLS_INSTALL_DIR/bin/$command"
done
