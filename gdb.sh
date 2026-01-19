#!/bin/bash

set -e

mkdir -p tmp
cmdfile=tmp/gdb.cmd

cat >$cmdfile <<EOF
symbol-file src/kernel/kernel.exe
target remote :1234
b do_exit
c
EOF
gdb --command=$cmdfile
