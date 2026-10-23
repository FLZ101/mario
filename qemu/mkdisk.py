#!/usr/bin/env python3
"""Rootless raw-disk builder shared by make-hd1/2/3.

The images are assembled without losetup or mount(8).  The builder:

  1. creates an empty image,
  2. writes the partition table with sfdisk, letting sfdisk choose the
     sector offsets,
  3. reads those offsets back from ``sfdisk --json``,
  4. creates one standalone filesystem image per partition and populates it
     with mtools (FAT) or debugfs (ext2/3/4),
  5. splices each filesystem into the final image at the offset sfdisk chose.

No sector offsets are hardcoded; they always come from the partition table
that sfdisk wrote.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

SECTOR_SIZE = 512
# sfdisk aligns partitions to 1 MiB by default; it also inserts this many
# sectors between consecutive logical partitions (the EBR / link sectors).
ALIGN_SECTORS = 2048

EXTENDED_TYPE = "5"

# DOS MBR type id for each filesystem this project uses.
FSTYPE_TO_DOS_TYPE = {
    "fat32": "0b",
    "ext2": "83",
    "ext4": "83",
}

# GPT partition type GUIDs.  hd3 uses two different FAT partitions, so the
# GUID cannot be derived from the filesystem type alone and is passed
# explicitly by the caller.
GPT_TYPE_GUIDS = {
    "efi-system": "C12A7328-F81F-11D2-BA4B-00A0C93EC93B",
    "microsoft-basic-data": "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
    "linux-filesystem": "0FC63DAF-8483-4772-8E79-3D69D8477DE4",
}


@dataclass
class Partition:
    """One partition of a disk image.

    ``fstype`` is None for the DOS extended container.  ``size_mib`` is
    None for that container as well; the builder computes its size from the
    logical partitions when needed.
    """

    number: Optional[int]
    size_mib: Optional[int]
    fstype: Optional[str]
    type_id: Optional[str] = None
    bootable: bool = False
    content: tuple = field(default_factory=tuple)


def extended() -> Partition:
    """Return the DOS extended-container partition marker."""
    return Partition(number=None, size_mib=None, fstype=None)


@dataclass
class Disk:
    """A complete disk image description."""

    name: str
    size_mib: int
    label: str
    partitions: list
    boot_mbr: Optional[Path] = None


def _run(cmd, check=True, **kwargs):
    return subprocess.run(cmd, check=check, **kwargs)


def _quiet(cmd):
    return _run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def _size_sectors(partition):
    return partition.size_mib * (1024 * 1024 // SECTOR_SIZE)


def _is_primary(p):
    return p.fstype is not None and p.number is not None and p.number <= 4


def _is_logical(p):
    return p.fstype is not None and p.number is not None and p.number >= 5


def partition_script(disk):
    """Build the sfdisk script from the disk spec (no sector math here)."""
    lines = ["label: " + disk.label, "unit: sectors"]

    extended_indexes = [i for i, p in enumerate(disk.partitions) if p.fstype is None]
    extended_size = None
    if extended_indexes:
        ext_index = extended_indexes[0]
        logicals = [p for p in disk.partitions if _is_logical(p)]
        late_primaries = [p for p in disk.partitions[ext_index + 1 :] if _is_primary(p)]
        # If a primary sits after the extended container it needs its own
        # room, so size the container to exactly fit the logical partitions.
        if late_primaries:
            extended_size = sum(_size_sectors(p) + ALIGN_SECTORS for p in logicals)

    for p in disk.partitions:
        if p.fstype is None:
            fields = []
            if extended_size is not None:
                fields.append("size=" + str(extended_size))
            fields.append("type=" + EXTENDED_TYPE)
            lines.append(", ".join(fields))
            continue

        type_id = p.type_id or FSTYPE_TO_DOS_TYPE[p.fstype]
        fields = ["%d: size=%dMiB" % (p.number, p.size_mib), "type=" + type_id]
        if p.bootable:
            fields.append("bootable")
        lines.append(", ".join(fields))

    return "\n".join(lines) + "\n"


def read_layout(image):
    """Read sfdisk's chosen offsets for every data partition."""
    result = _quiet(["sfdisk", "-J", str(image)])
    table = json.loads(result.stdout)
    layout = {}
    for entry in table["partitiontable"]["partitions"]:
        match = re.search(r"(\d+)$", entry["node"])
        number = int(match.group(1))
        layout[number] = {
            "start": int(entry["start"]),
            "size": int(entry["size"]),
        }
    return layout


def make_fs(fstype, image):
    if fstype == "fat32":
        _quiet(["mkfs.vfat", "-F", "32", str(image)])
    elif fstype == "ext2":
        _quiet(["mkfs.ext2", "-q", "-F", str(image)])
    elif fstype == "ext4":
        _quiet(["mkfs.ext4", "-q", "-F", str(image)])
    else:
        raise ValueError("unknown filesystem: " + fstype)


def _fat_mkdirs(image, parts):
    current = ""
    for part in parts:
        current += "/" + part
        _run(
            ["mmd", "-i", str(image), "::" + current],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )


def _fat_target(dest):
    dest = dest.rstrip("/")
    return "::" + (dest + "/" if dest else "/")


def populate_fat(image, content):
    for source, dest in content:
        source = Path(source)
        if source.is_dir():
            for path in sorted(source.rglob("*")):
                if not path.is_file():
                    continue
                rel = path.relative_to(source)
                parent = str(rel.parent)
                if parent != ".":
                    _fat_mkdirs(image, rel.parent.parts)
                    target = _fat_target(dest + "/" + parent)
                else:
                    target = _fat_target(dest)
                _quiet(["mcopy", "-i", str(image), str(path), target])
        else:
            _quiet(["mcopy", "-i", str(image), str(source), _fat_target(dest)])


def _ext_mkdirs(image, parts):
    current = ""
    for part in parts:
        current += "/" + part
        _run(
            ["debugfs", "-w", "-R", "mkdir " + current, str(image)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )


def populate_ext(image, content):
    for source, dest in content:
        source = Path(source)
        dest = dest.rstrip("/")
        if source.is_dir():
            for path in sorted(source.rglob("*")):
                if not path.is_file():
                    continue
                rel = path.relative_to(source)
                parent = str(rel.parent)
                if parent != ".":
                    _ext_mkdirs(image, rel.parent.parts)
                _quiet(
                    [
                        "debugfs",
                        "-w",
                        "-R",
                        "write %s %s/%s" % (path, dest, rel),
                        str(image),
                    ]
                )
        else:
            _quiet(
                [
                    "debugfs",
                    "-w",
                    "-R",
                    "write %s %s/%s" % (source, dest, source.name),
                    str(image),
                ]
            )


def _dd_zero(image, bs, count):
    _quiet(
        [
            "dd",
            "if=/dev/zero",
            "of=" + str(image),
            "bs=" + bs,
            "count=" + str(count),
            "status=none",
        ]
    )


def _dd_splice(src, dst, seek):
    _quiet(
        [
            "dd",
            "if=" + str(src),
            "of=" + str(dst),
            "bs=512",
            "seek=" + str(seek),
            "conv=notrunc",
            "status=none",
        ]
    )


def install_mbr_bootcode(image, grldr_mbr, tmp):
    """Overlay the real partition table onto grldr.mbr and write it to MBR."""
    _quiet(["cp", str(grldr_mbr), str(tmp)])
    _quiet(
        [
            "dd",
            "if=" + str(image),
            "of=" + str(tmp),
            "bs=1",
            "skip=440",
            "seek=440",
            "count=72",
            "conv=notrunc",
            "status=none",
        ]
    )
    _quiet(["dd", "if=" + str(tmp), "of=" + str(image), "conv=notrunc", "status=none"])


def expand_deps(disk):
    files = set()
    for partition in disk.partitions:
        for source, _ in partition.content:
            source = Path(source)
            if source.is_dir():
                files.update(p for p in source.rglob("*") if p.is_file())
            else:
                files.add(source)
    if disk.boot_mbr is not None:
        files.add(Path(disk.boot_mbr))
    return files


def build(disk):
    here = Path.cwd()
    image = here / (disk.name + ".img")
    tmp = here / "tmp"
    tmp.mkdir(exist_ok=True)

    print("Create %s.img ..." % disk.name, flush=True)
    _dd_zero(image, "1M", disk.size_mib)

    print("Partition %s.img ..." % disk.name, flush=True)
    _run(
        ["sfdisk", str(image)],
        input=partition_script(disk),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    layout = read_layout(image)
    data = [p for p in disk.partitions if p.fstype is not None]

    print("Format partitions of %s.img ..." % disk.name, flush=True)
    fs_images = {}
    for p in data:
        fs = tmp / ("%s.p%d.%s" % (disk.name, p.number, p.fstype))
        _dd_zero(fs, "512", layout[p.number]["size"])
        make_fs(p.fstype, fs)
        fs_images[p.number] = fs

    print("Populate %s.img ..." % disk.name, flush=True)
    for p in data:
        if p.fstype == "fat32":
            populate_fat(fs_images[p.number], p.content)
        else:
            populate_ext(fs_images[p.number], p.content)

    print("Assemble %s.img ..." % disk.name, flush=True)
    for p in data:
        _dd_splice(fs_images[p.number], image, layout[p.number]["start"])

    if disk.boot_mbr is not None:
        print("Install boot code into %s.img ..." % disk.name, flush=True)
        install_mbr_bootcode(image, disk.boot_mbr, tmp / "grldr.mbr")

    _run(["sfdisk", "-l", str(image)])


def main(disk):
    parser = argparse.ArgumentParser()
    parser.add_argument("--deps", action="store_true")
    args = parser.parse_args()

    if args.deps:
        for path in sorted(expand_deps(disk)):
            print(path)
        return 0

    build(disk)
    return 0
