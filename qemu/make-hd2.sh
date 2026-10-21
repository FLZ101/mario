#!/usr/bin/env bash

set -e

name=hd2
loopdev=""

FILES=$(find ./$name -type f 2>/dev/null || true)

print_deps()
{
	printf '%s\n' \
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
	dd if=/dev/zero of=$name.img bs=1024k count=320

	echo "Partition $name.img ..."
	sfdisk $name.img <<EOF
unit: sectors
label: dos
1: start=2048, size=131072, type=83
2: start=133120, size=266240, type=5
3: start=399360, size=131072, type=0b
5: start=135168, size=131072, type=0b
6: start=268288, size=131072, type=83
EOF

	echo "Format partitions of $name.img ..."

	loopdev=$(losetup --find)
	if [[ ! $loopdev =~ ^/dev/.* ]] ; then
		false
	fi

	sudo losetup -P $loopdev $name.img

    sudo mkfs -t ext2 ${loopdev}p1
    sudo mkfs -t fat -F 32 ${loopdev}p3
    sudo mkfs -t fat -F 32 ${loopdev}p5
    sudo mkfs -t ext2 ${loopdev}p6

	# ------
	echo "Populate $name.img ..."

	mkdir -p tmp/{p1,p3,p5,p6}
	sudo mount ${loopdev}p1 tmp/p1
	sudo mount ${loopdev}p3 tmp/p3
	sudo mount ${loopdev}p5 tmp/p5
	sudo mount ${loopdev}p6 tmp/p6

	sudo cp -r $name/* tmp

	sudo umount tmp/{p1,p3,p5,p6}

    sudo losetup --detach $loopdev

	sfdisk -l $name.img
}

create_hd
