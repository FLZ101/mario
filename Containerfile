FROM fedora:39

RUN dnf install -y \
    make gcc binutils nasm qemu-system-x86 gdb \
    autoconf automake libtool bison yacc \
    git tmux vim wget patch socat unzip tar gzip dosfstools e2fsprogs mtools \
    # For building Python
    zlib-devel \
    # For CP437
    glibc-gconv-extra \
    bash-completion && dnf clean all
