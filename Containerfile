FROM fedora:39

RUN dnf install -y \
    make gcc binutils nasm qemu-system-x86 gdb \
    autoconf automake libtool \
    git tmux vim wget socat unzip tar gzip dosfstools e2fsprogs \
    # For building Python
    zlib-devel \
    # For CP437
    glibc-gconv-extra \
    bash-completion && dnf clean all
