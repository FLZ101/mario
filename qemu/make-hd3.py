#!/usr/bin/env python3

import os
from pathlib import Path

import mkdisk

disk = mkdisk.Disk(
    name="hd3",
    size_mib=320,
    label="gpt",
    partitions=[
        mkdisk.Partition(
            1,
            64,
            "fat32",
            type_id=mkdisk.GPT_TYPE_GUIDS["efi-system"],
            content=((Path("hd3/p1"), "/"),),
        ),
        mkdisk.Partition(
            2,
            64,
            "fat32",
            type_id=mkdisk.GPT_TYPE_GUIDS["microsoft-basic-data"],
            content=((Path("hd3/p2"), "/"),),
        ),
        mkdisk.Partition(
            3,
            64,
            "ext2",
            type_id=mkdisk.GPT_TYPE_GUIDS["linux-filesystem"],
            content=((Path("hd3/p3"), "/"),),
        ),
        mkdisk.Partition(
            4,
            64,
            "ext2",
            type_id=mkdisk.GPT_TYPE_GUIDS["linux-filesystem"],
            content=((Path("hd3/p4"), "/"),),
        ),
    ],
)

if __name__ == "__main__":
    os.chdir(Path(__file__).resolve().parent)
    raise SystemExit(mkdisk.main(disk))
