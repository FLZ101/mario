#!/usr/bin/env bash

set -e

name=hd1
loopdev=""

GRUB=grub4dos-0.4.6a
GRUB_FILES=$(find $GRUB -mindepth 1 -maxdepth 1 -type f 2>/dev/null || true)
BOOT_BIN=../src/boot/boot.bin
KERNEL_EXE=../src/kernel/kernel.exe
RDX=../rd/rdx

FILES=$(find ./$name -type f 2>/dev/null || true)

print_deps()
{
	printf '%s\n' \
		$BOOT_BIN \
		$RDX \
		$KERNEL_EXE \
		$GRUB_FILES \
		$FILES
}

if [[ "${1:-}" == --deps ]] ; then
	print_deps
	exit 0
fi

mkdir -p tmp

cleanup()
{
	for x in tmp/*/ ; do
		sudo umount "$x" 2>/dev/null || true
	done

	if [[ -n "$loopdev" ]] ; then
		sudo losetup --detach "$loopdev" 2>/dev/null || true
	fi
}
trap cleanup EXIT

create_hd()
{
	echo "Create $name.img ..."
	dd if=/dev/zero of=$name.img bs=1024k count=400

	echo "Partition $name.img ..."
	sfdisk $name.img <<EOF
unit: sectors
label: dos
size=100MiB, bootable, type=0b
type=5
size=100MiB, type=83
size=50MiB, type=83
size=100MiB, type=0b
EOF

	# ------
	echo "Format partitions of $name.img ..."

	loopdev=$(losetup --find)
	if [[ ! $loopdev =~ ^/dev/.* ]] ; then
		return 1
	fi

	sudo losetup -P $loopdev $name.img

	sudo mkfs -t fat -F 32 ${loopdev}p1
	sudo mkfs -t ext2 ${loopdev}p5
	sudo mkfs -t ext4 ${loopdev}p6
	sudo mkfs -t fat -F 32 ${loopdev}p7

	# ------
	echo "Populate $name.img ..."

	mkdir -p tmp/{p1,p5,p6,p7}
	sudo mount ${loopdev}p1 tmp/p1
	sudo mount ${loopdev}p5 tmp/p5
	sudo mount ${loopdev}p6 tmp/p6
	sudo mount ${loopdev}p7 tmp/p7

	sudo cp $GRUB/{grldr,menu.lst} tmp/p1
	sudo cp $KERNEL_EXE $BOOT_BIN $RDX tmp/p7

	sudo cp -r $name/* tmp

	sudo umount tmp/{p1,p5,p6,p7}

	cp $GRUB/grldr.mbr tmp/
	# copy the partition table
	sudo dd if=${loopdev} of=tmp/grldr.mbr bs=1 count=72 skip=440 seek=440 conv=notrunc
	# overwrite MBR
	sudo dd if=tmp/grldr.mbr of=${loopdev} conv=notrunc

	sudo losetup --detach $loopdev

	sfdisk -l $name.img
}

create_hd
