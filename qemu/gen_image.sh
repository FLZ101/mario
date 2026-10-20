#!/usr/bin/env bash

set -e

loopdev=""
loopdev2=""
loopdev3=""

cleanup()
{
	sudo umount tmp/p1 2>/dev/null || true
	sudo umount tmp/p6 2>/dev/null || true

	if [[ -n "$loopdev" ]] ; then
		sudo losetup --detach "$loopdev" 2>/dev/null || true
	fi
	if [[ -n "$loopdev2" ]] ; then
		sudo losetup --detach "$loopdev2" 2>/dev/null || true
	fi
	if [[ -n "$loopdev3" ]] ; then
		sudo losetup --detach "$loopdev3" 2>/dev/null || true
	fi
}
trap cleanup EXIT

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

echo "Create hd2.img ..."
dd if=/dev/zero of=hd2.img bs=1024k count=320

echo "Partition hd2.img ..."
sfdisk hd2.img <<EOF
unit: sectors
label: dos
1: start=2048, size=131072, type=83
2: start=133120, size=266240, type=5
3: start=399360, size=131072, type=0b
5: start=135168, size=131072, type=0b
6: start=268288, size=131072, type=83
EOF

echo "Format partitions of hd2.img ..."

loopdev2=$(losetup --find)
if [[ ! $loopdev2 =~ ^/dev/.* ]] ; then
	false
fi

sudo losetup -P $loopdev2 hd2.img

sudo mkfs -t ext2 ${loopdev2}p1
sudo mkfs -t fat -F 32 ${loopdev2}p3
sudo mkfs -t fat -F 32 ${loopdev2}p5
sudo mkfs -t ext2 ${loopdev2}p6

sudo losetup --detach $loopdev2

echo "Create hd3.img ..."
dd if=/dev/zero of=hd3.img bs=1024k count=320

echo "Partition hd3.img ..."
sfdisk hd3.img <<EOF
label: gpt
unit: sectors
1: start=2048, size=131072, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
2: start=133120, size=131072, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
3: start=264192, size=131072, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
4: start=395264, size=131072, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
EOF

echo "Format partitions of hd3.img ..."

loopdev3=$(losetup --find)
if [[ ! $loopdev3 =~ ^/dev/.* ]] ; then
	false
fi

sudo losetup -P $loopdev3 hd3.img

sudo mkfs -t fat -F 32 ${loopdev3}p1
sudo mkfs -t fat -F 32 ${loopdev3}p2
sudo mkfs -t ext2 ${loopdev3}p3
sudo mkfs -t ext2 ${loopdev3}p4

sudo losetup --detach $loopdev3

sfdisk -l hd.img
sfdisk -l hd2.img
sfdisk -l hd3.img

echo "Done"
