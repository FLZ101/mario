#!/bin/bash

set -e

mkdir -p tmp
cmdfile=tmp/gdb.cmd

cat >$cmdfile <<EOF
symbol-file src/kernel/kernel.exe
add-symbol-file app/init/init.exe
target remote :1234
b mario
b pipe_read
# b fs/exec.c:290  
c
EOF
gdb --command=$cmdfile
