#!/bin/bash

set -e

mkdir -p tmp
cmdfile=tmp/gdb.cmd

cat >$cmdfile <<EOF
symbol-file src/kernel/kernel.exe
add-symbol-file app/example/init/init.exe
target remote :1234
# b mario
b main
b do_fork
b do_execve
# b *0x00408e4f
# b mario
# b _start
# b __libc_start_main
# b __init_tls
# b sys_set_thread_area
# b *0x40235e
# b __init_ssp
# b fs/exec.c:290
c
EOF
gdb --command=$cmdfile
