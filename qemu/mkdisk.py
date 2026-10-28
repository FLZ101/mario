#!/usr/bin/env python3
"""Shared raw-disk builder used by make-hd1/2/3.

The image is assembled with Linux loop devices:

  1. create an empty image,
  2. write the partition table with sfdisk,
  3. attach the image as a partitioned loop device,
  4. create one filesystem on each data partition,
  5. mount each partition and copy its contents into it, and
  6. optionally overlay grldr.mbr while preserving the partition table.

Using ``losetup --partscan`` means we do not hardcode partition byte
offsets.  The kernel derives the /dev/loopNpM nodes from the partition
table that sfdisk wrote.

``--deps`` is handled before any loop or mount operation, so dependency
scanning remains safe to run on ordinary hosts.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

SECTOR_SIZE = 512
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
    None for that container as well; sfdisk computes its size from the
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
    """Run a command, returning the completed process."""
    return subprocess.run(cmd, check=check, **kwargs)


def _quiet(cmd, check=True):
    """Run a command and capture its stdout/stderr as text."""
    return _run(
        cmd,
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def _root_cmd(cmd):
    """Prefix commands that require root with sudo unless already root."""
    if os.geteuid() == 0:
        return cmd
    return ["sudo", *cmd]


def _size_sectors(partition):
    """Return the number of 512-byte sectors in a data partition."""
    return partition.size_mib * (1024 * 1024 // SECTOR_SIZE)


def _is_primary(p):
    return p.fstype is not None and p.number is not None and p.number <= 4


def _is_logical(p):
    return p.fstype is not None and p.number is not None and p.number >= 5


def partition_script(disk):
    """Build the sfdisk script from the disk spec."""
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


def _create_empty_image(image: Path, size_bytes: int) -> None:
    """Create (or replace) the raw image as a sparse file."""
    with open(image, "wb") as stream:
        stream.truncate(size_bytes)


def _attach_loop(image: Path) -> str:
    """Attach ``image`` to a fresh partitioned loop device."""
    result = _quiet(
        _root_cmd(["losetup", "--partscan", "--find", "--show", str(image)])
    )
    loop = result.stdout.strip()
    if not loop:
        raise RuntimeError("losetup did not report a loop device")
    return loop


def _wait_for_partition_devices(loop: str, partitions) -> None:
    """Wait for the kernel to create the loopNpM device nodes."""
    missing = ["%sp%d" % (loop, p.number) for p in partitions]
    deadline = time.monotonic() + 10
    while missing and time.monotonic() < deadline:
        missing = [device for device in missing if not Path(device).exists()]
        if missing:
            time.sleep(0.05)
    if missing:
        raise RuntimeError("loop partition devices did not appear: " + ", ".join(missing))


def _make_filesystem(fstype: str, device: str) -> None:
    if fstype == "fat32":
        _quiet(_root_cmd(["mkfs.vfat", "-F", "32", device]))
    elif fstype == "ext2":
        _quiet(_root_cmd(["mkfs.ext2", "-q", "-F", device]))
    elif fstype == "ext4":
        _quiet(_root_cmd(["mkfs.ext4", "-q", "-F", device]))
    else:
        raise ValueError("unknown filesystem: " + fstype)


def _copy_entry(fstype: str, source: Path, dest_root: Path) -> None:
    """Copy a file or directory into ``dest_root`` using cp(1)."""
    dest = str(dest_root) + "/"

    if source.is_dir():
        # ``source/.`` copies the contents of the directory, rather than
        # creating ``dest_root/source.name``.
        if fstype == "fat32":
            _quiet(["cp", "-r", str(source) + "/.", dest])
        else:
            _quiet(["cp", "-a", str(source) + "/.", dest])
    else:
        if fstype == "fat32":
            _quiet(["cp", str(source), dest])
        else:
            _quiet(["cp", "-a", str(source), dest])


def _populate_filesystem(fstype: str, content: tuple, mountpoint: Path) -> None:
    """Copy all content entries into a mounted filesystem."""
    for source, dest in content:
        source = Path(source)
        dest_root = mountpoint / dest.strip("/")
        dest_root.mkdir(parents=True, exist_ok=True)

        _copy_entry(fstype, source, dest_root)


def _umount_quiet(mountpoint: Path) -> None:
    """Unmount a mountpoint if possible, without masking other errors."""
    _run(_root_cmd(["umount", str(mountpoint)]), check=False)


def _detach_loop_quiet(loop: str) -> None:
    _run(_root_cmd(["losetup", "-d", loop]), check=False)


def install_mbr_bootcode(image: Path, grldr_mbr: Path) -> None:
    """Overlay the real MBR partition table onto grldr.mbr.

    grldr.mbr supplies the first 440 bytes of boot code; the remaining bytes
    (disk signature, partition table and boot signature) must come from the
    image that sfdisk created.
    """
    with open(image, "rb") as stream:
        image_mbr = bytearray(stream.read(512))

    boot_code = bytearray(grldr_mbr.read_bytes())
    if len(boot_code) < 512:
        boot_code.extend(b"\x00" * (512 - len(boot_code)))

    # Keep the complete grldr.mbr image.  It is normally 16 sectors: sector 0
    # contains the MBR boot code and sectors 1-15 contain the helper code
    # that finds and loads GRLDR.  Only the partition table needs to be
    # replaced from the image.
    boot_code[440:512] = image_mbr[440:512]
    with open(image, "r+b") as stream:
        stream.seek(0)
        stream.write(boot_code)


def expand_deps(disk: Disk):
    """Return every host file that ``disk`` copies into the image."""
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


def build(disk: Disk) -> None:
    here = Path.cwd()
    image = here / (disk.name + ".img")
    tmp = here / "tmp"
    tmp.mkdir(exist_ok=True)

    data_partitions = [p for p in disk.partitions if p.fstype is not None]
    loop = None
    mount_root = None

    try:
        print("Create %s.img ..." % disk.name, flush=True)
        _create_empty_image(image, disk.size_mib * 1024 * 1024)

        print("Partition %s.img ..." % disk.name, flush=True)
        _run(
            ["sfdisk", str(image)],
            input=partition_script(disk),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        print("Attach loop device ...", flush=True)
        loop = _attach_loop(image)
        print("  %s" % loop, flush=True)
        _wait_for_partition_devices(loop, data_partitions)

        mount_root = Path(tempfile.mkdtemp(prefix=disk.name + ".", dir=tmp))

        print("Format partitions of %s.img ..." % disk.name, flush=True)
        for partition in data_partitions:
            device = "%sp%d" % (loop, partition.number)
            print("  format %s as %s" % (device, partition.fstype), flush=True)
            _make_filesystem(partition.fstype, device)

        print("Populate %s.img ..." % disk.name, flush=True)
        for partition in data_partitions:
            device = "%sp%d" % (loop, partition.number)
            mountpoint = mount_root / ("p%d" % partition.number)
            mountpoint.mkdir(parents=True, exist_ok=True)
            print("  mount %s" % device, flush=True)
            if partition.fstype == "fat32":
                _quiet(
                    _root_cmd(
                        [
                            "mount",
                            "-o",
                            "uid=%d,gid=%d" % (os.getuid(), os.getgid()),
                            device,
                            str(mountpoint),
                        ]
                    )
                )
            else:
                _quiet(_root_cmd(["mount", device, str(mountpoint)]))
                # Make the ext root directory writable by the current user so
                # the unprivileged cp commands can populate it.
                _quiet(
                    _root_cmd(
                        [
                            "chown",
                            "%d:%d" % (os.getuid(), os.getgid()),
                            str(mountpoint),
                        ]
                    )
                )
            try:
                _populate_filesystem(partition.fstype, partition.content, mountpoint)
            finally:
                _umount_quiet(mountpoint)
    finally:
        if loop is not None:
            _detach_loop_quiet(loop)
        if mount_root is not None:
            shutil.rmtree(mount_root, ignore_errors=True)

    if disk.boot_mbr is not None:
        print("Install boot code into %s.img ..." % disk.name, flush=True)
        install_mbr_bootcode(image, disk.boot_mbr)

    _run(["sfdisk", "-l", str(image)])


def main(disk: Disk) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--deps", action="store_true")
    args = parser.parse_args()

    if args.deps:
        for path in sorted(expand_deps(disk)):
            print(path)
        return 0

    build(disk)
    return 0
