#!/bin/bash

set -e

mkdir -p tmp
cmdfile=tmp/gdb.cmd

cat >$cmdfile <<EOF
symbol-file src/kernel/kernel.exe
# add-symbol-file app/example/init/init.exe
# add-symbol-file app/usr/vim-9.2.0428-install/bin/vim

target remote :1234

b mario

# b main
# b _start
# b __libc_start_main

# b *0x40235e
# b fs/exec.c:290

c
EOF
gdb --command=$cmdfile
