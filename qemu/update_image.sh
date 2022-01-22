#!/usr/bin/env bash

set -e

echo "Update hd.img ..."

loopdev=$(losetup --find)
if [[ ! $loopdev =~ ^/dev/.* ]] ; then
	false
fi

sudo losetup -P $loopdev hd.img

mkdir -p tmp/p6
sudo mount ${loopdev}p6 tmp/p6

sudo cp tmp/{kernel.exe,boot.bin,rd0,rd1,rd2} tmp/p6

sudo umount tmp/p6

sudo losetup --detach $loopdev

echo "Done"
