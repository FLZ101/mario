"""Build the standard or GTK root ramdisk."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import copy_files


RD_DIR = Path(__file__).resolve().parent


def write_rd0(staging: Path, image: str) -> None:
    subprocess.run(
        [
            str(RD_DIR / "rd-make.exe"),
            f"--dir={staging}",
            f"--rd={image}",
            "--device-list=device_list",
            "--nr-block=5120",
        ],
        cwd=RD_DIR,
        check=True,
    )


def build_rd0() -> None:
    staging = RD_DIR / "ramdisk0"
    shutil.rmtree(staging, ignore_errors=True)

    copy_files.copy_rootfs_files(staging)

    usr = RD_DIR / "ramdisk0_usr"
    shutil.rmtree(usr, ignore_errors=True)
    os.rename(staging / "usr", usr)

    # init mounts the separately built ramdisk0_usr image at /usr, so the
    # root ramdisk still needs an empty mountpoint for that directory.
    (staging / "usr").mkdir(parents=True, exist_ok=True)
    write_rd0(staging, "rd0")


def copy_overlay(source: Path, target: Path) -> None:
    """Merge directories and replace other entries without following symlinks."""
    if not source.is_symlink() and source.is_dir():
        if target.is_symlink() or (target.exists() and not target.is_dir()):
            target.unlink()
        target.mkdir(exist_ok=True)
        for child in source.iterdir():
            copy_overlay(child, target / child.name)
        shutil.copystat(source, target)
    else:
        if not target.is_symlink() and target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink(missing_ok=True)
        shutil.copy2(source, target, follow_symlinks=False)


def build_gtk_rd0() -> None:
    overlay = RD_DIR / "_ramdisk0-gtk"
    with tempfile.TemporaryDirectory(prefix="ramdisk0-gtk-", dir=RD_DIR) as directory:
        work = Path(directory)
        staging = work / "root"
        shutil.copytree(RD_DIR / "ramdisk0", staging, symlinks=True)
        shutil.copytree(
            RD_DIR / "ramdisk0_usr", staging / "usr", dirs_exist_ok=True, symlinks=True
        )
        if overlay.exists():
            for entry in overlay.iterdir():
                copy_overlay(entry, staging / entry.name)

        usr = staging / "usr"
        if usr.is_symlink() or not usr.is_dir():
            raise ValueError("GTK overlay /usr must be a directory")
        staged_usr = usr.rename(work / "usr")
        usr.mkdir()
        image = work / "rd0-gtk"
        write_rd0(staging, str(image))

        gtk_usr = RD_DIR / "ramdisk0_usr-gtk"
        if gtk_usr.exists():
            shutil.rmtree(gtk_usr)
        staged_usr.rename(gtk_usr)
        os.utime(gtk_usr, None)
        image.replace(RD_DIR / image.name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--deps", action="store_true", help="print rd0 prerequisites for Make"
    )
    mode.add_argument("--gtk", action="store_true", help="build rd0-gtk from staged rd0 files")
    args = parser.parse_args(argv)

    if args.deps:
        copy_files.copy_rootfs_files(
            None,
            print_deps=True,
        )
        return 0

    if args.gtk:
        build_gtk_rd0()
    else:
        build_rd0()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
