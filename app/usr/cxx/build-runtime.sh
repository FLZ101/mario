#!/bin/bash

set -euo pipefail

ninja="$(dirname "$PYTHON")/ninja"
if [[ ! -x $PYTHON || ! -x $ninja ]] ; then
    echo "The build venv must contain Python and Ninja; run make venv first" >&2
    exit 1
fi

gcc_version=$(g++ -dumpversion)
if (( ${gcc_version%%.*} < 13 )); then
    echo "Mario's C++ runtime requires GCC 13 or later" >&2
    exit 1
fi

llvm_version=18.1.8
release="https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version"
raw="https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$llvm_version"
downloads="$CXX_DIR/downloads"
sources="$CXX_BUILD_DIR/llvm-src"
build="$CXX_BUILD_DIR/runtime"

mkdir -p "$downloads" "$sources"
cd "$downloads"
while read -r checksum file; do
    case "$file" in
        llvm/*) url="$raw/$file" ;;
        *) url="$release/$file" ;;
    esac
    if [[ ! -f $file ]]; then
        mkdir -p "$(dirname "$file")"
        wget --tries=3 "$url" -O "$file.tmp"
        printf '%s  %s\n' "$checksum" "$file.tmp" | sha256sum -c -
        mv "$file.tmp" "$file"
    else
        printf '%s  %s\n' "$checksum" "$file" | sha256sum -c -
    fi
    case "$file" in
        llvm/*) install -D "$file" "$sources/$file" ;;
        *)
            component=${file%-$llvm_version.src.tar.xz}
            mkdir -p "$sources/$component"
            tar xf "$file" --strip-components=1 -C "$sources/$component"
            ;;
    esac
done < "$CXX_DIR/llvm-sources.sha256"
patch -d "$sources/libcxx" -p1 < "$CXX_DIR/libcxx-gcc.patch"
patch -d "$sources/libcxx" -p1 < "$CXX_DIR/libcxx-mario.patch"
patch -d "$sources/libcxxabi" -p1 < "$CXX_DIR/libcxxabi-gcc.patch"

cd "$CXX_BUILD_DIR"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH CFLAGS CXXFLAGS CPPFLAGS LDFLAGS
export CC_FOR_BUILD=gcc
target_flags='-m32 -march=i686'
headers="-nostdinc -isystem \"$MUSL_DIR/include\" -isystem \"$(gcc -print-file-name=include)\""
compile_flags="$target_flags $headers -fno-stack-protector"

cmake -S "$sources/runtimes" -B "$build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" -DPython3_EXECUTABLE="$PYTHON" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=i386 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_ASM_COMPILER=gcc \
    -DCMAKE_C_FLAGS="$compile_flags" -DCMAKE_CXX_FLAGS="$compile_flags" \
    -DCMAKE_ASM_FLAGS="$target_flags" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_LLVM=ON -DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON \
    -DCMAKE_INSTALL_PREFIX="$CXX_INSTALL_DIR" -DCMAKE_INSTALL_LIBDIR=lib \
    -DLLVM_DEFAULT_TARGET_TRIPLE=i386-unknown-linux-musl \
    -DLLVM_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_ENABLE_RUNTIMES='libcxx;libcxxabi;libunwind' \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBUNWIND_ENABLE_SHARED=OFF \
    -DLIBCXX_ENABLE_EXCEPTIONS=ON -DLIBCXXABI_ENABLE_EXCEPTIONS=ON \
    -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_HAS_PTHREAD_LIB=OFF \
    -DLIBCXX_HAS_RT_LIB=OFF -DLIBCXX_HAS_ATOMIC_LIB=OFF \
    -DLIBCXX_USE_COMPILER_RT=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
    -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBUNWIND_HAS_GCC_S_LIB=OFF -DLIBUNWIND_HAS_GCC_LIB=OFF
cmake --build "$build" --parallel "${JOBS:-1}"
cmake --install "$build"
