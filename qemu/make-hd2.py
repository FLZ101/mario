#!/usr/bin/env python3

import os
from pathlib import Path

import mkdisk

disk = mkdisk.Disk(
    name="hd2",
    size_mib=320,
    label="dos",
    partitions=[
        mkdisk.Partition(1, 64, "ext2", content=((Path("hd2/p1"), "/"),)),
        mkdisk.extended(),
        mkdisk.Partition(3, 64, "fat32", content=((Path("hd2/p3"), "/"),)),
        mkdisk.Partition(5, 64, "fat32", content=((Path("hd2/p5"), "/"),)),
        mkdisk.Partition(6, 64, "ext2", content=((Path("hd2/p6"), "/"),)),
    ],
)

if __name__ == "__main__":
    os.chdir(Path(__file__).resolve().parent)
    raise SystemExit(mkdisk.main(disk))
