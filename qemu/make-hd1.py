#!/usr/bin/env python3

import os
from pathlib import Path

import mkdisk

GRUB = Path("grub4dos-0.4.6a")

disk = mkdisk.Disk(
    name="hd1",
    size_mib=400,
    label="dos",
    boot_mbr=GRUB / "grldr.mbr",
    partitions=[
        mkdisk.Partition(
            1,
            100,
            "fat32",
            bootable=True,
            content=(
                (GRUB / "grldr", "/"),
                (GRUB / "menu.lst", "/"),
                (Path("hd1/p1"), "/"),
            ),
        ),
        mkdisk.extended(),
        mkdisk.Partition(5, 100, "ext2", content=((Path("hd1/p5"), "/"),)),
        mkdisk.Partition(6, 50, "ext4", content=((Path("hd1/p6"), "/"),)),
        mkdisk.Partition(
            7,
            100,
            "fat32",
            content=(
                (Path("../src/kernel/kernel.exe"), "/"),
                (Path("../src/boot/boot.bin"), "/"),
                (Path("../rd/rdx"), "/"),
                (Path("hd1/p7"), "/"),
            ),
        ),
    ],
)

if __name__ == "__main__":
    os.chdir(Path(__file__).resolve().parent)
    raise SystemExit(mkdisk.main(disk))
