#!/bin/bash

set -e

mkdir -p tmp
cmdfile=tmp/gdb.cmd

cat >$cmdfile <<EOF
symbol-file src/kernel/kernel.exe
add-symbol-file app/init/init.exe
target remote :1234
b mario
# b init
# b sys_open
b sys_write
# b fs/exec.c:290  
c
EOF
gdb --command=$cmdfile
