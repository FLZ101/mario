#!/bin/bash

set -e

if [[ -z $CXX || -z $BTOP_SRC_DIR || -z $BTOP_INSTALL_DIR ]] ; then
    echo "CXX, BTOP_SRC_DIR and BTOP_INSTALL_DIR are not all set" >&2
    exit 1
fi

if [[ ! -x $PYTHON || ! -x $(dirname "$PYTHON")/python3 ]] ; then
    echo "The build venv must contain Python; run make venv first" >&2
    exit 1
fi

export PATH="$(dirname "$PYTHON"):$PATH"

pushd "$BTOP_SRC_DIR"

make PLATFORM=Linux ARCH=i686 GPU_SUPPORT=false STATIC=true GIT_COMMIT= \
    CXX="$CXX" THREADS="${JOBS:-1}" OPTFLAGS="${OPTIMIZE}" btop

install -Dm755 bin/btop "$BTOP_INSTALL_DIR/bin/btop"
install -Dm644 LICENSE "$BTOP_INSTALL_DIR/share/doc/btop/LICENSE"
themes="$BTOP_INSTALL_DIR/share/btop/themes"
mkdir -p "$themes"
install -m644 themes/*.theme "$themes"
touch "$themes"

popd
