#!/usr/bin/env bash

set -e

name=hd3
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
label: gpt
unit: sectors
1: start=2048, size=131072, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
2: start=133120, size=131072, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
3: start=264192, size=131072, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
4: start=395264, size=131072, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4
EOF

	echo "Format partitions of $name.img ..."

	loopdev=$(losetup --find)
	if [[ ! $loopdev =~ ^/dev/.* ]] ; then
		false
	fi

	sudo losetup -P $loopdev $name.img

	sudo mkfs -t fat -F 32 ${loopdev}p1
	sudo mkfs -t fat -F 32 ${loopdev}p2
	sudo mkfs -t ext2 ${loopdev}p3
	sudo mkfs -t ext2 ${loopdev}p4

	# ------
	echo "Populate $name.img ..."

	mkdir -p tmp/{p1,p2,p3,p4}
	sudo mount ${loopdev}p1 tmp/p1
	sudo mount ${loopdev}p2 tmp/p2
	sudo mount ${loopdev}p3 tmp/p3
	sudo mount ${loopdev}p4 tmp/p4

	sudo cp -r $name/* tmp

	sudo umount tmp/{p1,p2,p3,p4}

    sudo losetup --detach $loopdev

	sfdisk -l $name.img
}

create_hd
