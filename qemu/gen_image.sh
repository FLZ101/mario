#!/usr/bin/env bash

set -e

echo "Create hd.img ..."
dd if=/dev/zero of=hd.img bs=1024k count=400

echo "Partition hd.img ..."
sfdisk hd.img <<EOF
unit: sectors
label: dos
size=100MiB, bootable, type=0b
type=5
size=100MiB, type=83
size=50MiB, type=83
size=100MiB, type=0b
EOF

echo "Format partitions of hd.img ..."

loopdev=$(losetup --find)
if [[ ! $loopdev =~ ^/dev/.* ]] ; then
	false
fi

sudo losetup -P $loopdev hd.img

sudo mkfs -t fat -F 32 ${loopdev}p1
sudo mkfs -t ext2 ${loopdev}p5
sudo mkfs -t ext4 ${loopdev}p6
sudo mkfs -t fat -F 32 ${loopdev}p7

echo "Install grub4dos to hd.img ..."

cp grub4dos-0.4.6a/grldr.mbr tmp/

mkdir -p tmp/{p1,p6}
sudo mount ${loopdev}p1 tmp/p1
sudo mount ${loopdev}p6 tmp/p6

sudo cp grub4dos-0.4.6a/{grldr,menu.lst} tmp/p1

sudo umount tmp/{p1,p6}

# copy the partition table
sudo dd if=${loopdev} of=tmp/grldr.mbr bs=1 count=72 skip=440 seek=440 conv=notrunc
# overwrite MBR
sudo dd if=tmp/grldr.mbr of=${loopdev} conv=notrunc

sudo losetup --detach $loopdev

sfdisk -l hd.img

echo "Done"
