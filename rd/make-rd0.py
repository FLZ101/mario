#!/usr/bin/env python3
"""Build rd/rd0 from the static seed and the standard Mario rootfs files."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path

import copy_files


RD_DIR = Path(__file__).resolve().parent


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

    subprocess.run(
        [
            str(RD_DIR / "rd-make.exe"),
            "--dir=ramdisk0",
            "--rd=rd0",
            "--device-list=device_list",
            "--nr-block=5120",
        ],
        cwd=RD_DIR,
        check=True,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--deps", action="store_true", help="print rd0 prerequisites for Make"
    )
    args = parser.parse_args(argv)

    if args.deps:
        copy_files.copy_rootfs_files(
            None,
            print_deps=True,
        )
        return 0

    build_rd0()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
