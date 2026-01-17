#!/usr/bin/env bash

set -e

echo "Update hd.img ..."

loopdev=$(losetup --find)
if [[ ! $loopdev =~ ^/dev/.* ]] ; then
	false
fi

sudo losetup -P $loopdev hd.img

mkdir -p tmp/p7
sudo mount ${loopdev}p7 tmp/p7

sudo cp tmp/{kernel.exe,boot.bin,rd0,rd1,rd2} tmp/p7

sudo umount tmp/p7

sudo losetup --detach $loopdev

echo "Done"
