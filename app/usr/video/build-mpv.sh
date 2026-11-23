#!/bin/bash

set -e

if [[ -z $CC || -z $MPV_SRC_DIR || -z $PLACEBO_SRC_DIR || -z $MEDIA_BUILD_DIR || -z $MEDIA_INSTALL_DIR || -z $PCM_UAPI_DIR ]] ; then
    echo "CC, MPV_SRC_DIR, PLACEBO_SRC_DIR, MEDIA_BUILD_DIR, MEDIA_INSTALL_DIR and PCM_UAPI_DIR are not all set"
    exit 1
fi

meson="$(dirname "$PYTHON")/meson"
ninja="$(dirname "$PYTHON")/ninja"
if [[ ! -x $PYTHON || ! -x $meson || ! -x $ninja ]] ; then
    echo "The build venv must contain Python, Meson and Ninja; run make venv first" >&2
    exit 1
fi

unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
export NINJA="$ninja"
export GIT_CEILING_DIRECTORIES="$(dirname "$MPV_SRC_DIR")"
export LC_ALL=C
mkdir -p "$MEDIA_BUILD_DIR"
pushd "$MEDIA_BUILD_DIR"

cat > mario.cross <<EOF
[binaries]
c = '$CC'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
python = '$PYTHON'
python3 = '$PYTHON'

[host_machine]
system = 'mario'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$MEDIA_INSTALL_DIR/lib/pkgconfig'

[built-in options]
c_args = ['-I$PCM_UAPI_DIR', '-idirafter', '$(gcc -print-file-name=include)', '-march=i686', '-mno-mmx', '-mno-sse', '-mno-sse2', '-ffunction-sections', '-fdata-sections']
EOF

options=(--cross-file mario.cross --wrap-mode=nofallback --auto-features=disabled
    --default-library=static --prefer-static --buildtype=release
    --prefix="$MEDIA_INSTALL_DIR" --libdir=lib)

"$meson" setup placebo "$PLACEBO_SRC_DIR" "${options[@]}" -Ddemos=false
"$meson" compile -C placebo -j "${JOBS:-1}"
"$meson" install -C placebo --no-rebuild

"$meson" setup mpv "$MPV_SRC_DIR" "${options[@]}" \
    -Dlibmpv=false -Dlibass=disabled -Dgl=disabled -Dbuild-date=false
"$meson" compile -C mpv -j "${JOBS:-1}"

elf=$(readelf -W -h -l -d mpv/mpv)
if ! [[ $elf =~ Class:[[:space:]]+ELF32 && $elf =~ Machine:[[:space:]]+Intel\ 80386 ]] ||
        [[ $elf =~ (INTERP|NEEDED|DYNAMIC) ]]; then
    echo "Expected a static ELF32 i386 mpv executable" >&2
    exit 1
fi

install -Dm755 mpv/mpv "$MEDIA_INSTALL_DIR/bin/mpv.tmp"
mv "$MEDIA_INSTALL_DIR/bin/mpv.tmp" "$MEDIA_INSTALL_DIR/bin/mpv"

popd
