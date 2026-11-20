import os
from pathlib import Path

import mkdisk

gtk = os.environ.get("MARIO_HD3_GTK") == "1"
usr = Path("../rd/ramdisk0_usr-gtk" if gtk else "../rd/ramdisk0_usr")

disk = mkdisk.Disk(
    name="hd3-gtk" if gtk else "hd3",
    size_mib=320 + 512,
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
        mkdisk.Partition(
            5,
            512,
            "ext2",
            type_id=mkdisk.GPT_TYPE_GUIDS["linux-filesystem"],
            content=((usr, "/"),),
        ),
    ],
)

if __name__ == "__main__":
    os.chdir(Path(__file__).resolve().parent)
    raise SystemExit(mkdisk.main(disk))
