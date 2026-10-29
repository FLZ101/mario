#!/usr/bin/env python3
"""Copy files into a ramdisk staging tree.

The copy operations shared by the ramdisk builder live here so they can be
reused by other image builders without duplicating shell `cp` logic.
"""

from __future__ import annotations

import os
import shutil
import glob
import subprocess
import stat

from pathlib import Path
from typing import Mapping


ROOT = Path(__file__).resolve().parent.parent


def _needs_update(src: Path, dst: Path) -> bool:
    """Return True when dst is missing or older than src."""

    if not dst.exists():
        return True
    if src.is_symlink() or dst.is_symlink():
        return True
    if src.is_dir():
        return False
    return src.stat().st_mtime_ns > dst.stat().st_mtime_ns


def _copy_symlink(src: Path, dst: Path) -> None:
    target = os.readlink(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_symlink() and os.readlink(dst) == target:
        return
    if dst.exists() or dst.is_symlink():
        dst.unlink()
    os.symlink(target, dst)


def _is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"

def _make_writable(path):
    mode = os.stat(path).st_mode

    # Check whether the owner write permission is missing
    if not mode & stat.S_IWUSR:
        os.chmod(path, mode | stat.S_IWUSR)
        return True  # Changed to writable

    return False  # Already writable

def _strip_path(path: Path) -> None:
    if _is_elf(path):
        _make_writable(path)
        subprocess.run(["strip", "--strip-all", str(path)], check=False)


def _copy_file(src: Path, dst: Path) -> None:
    if dst.exists() and dst.is_dir():
        dst = dst / src.name
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not _needs_update(src, dst):
        return
    shutil.copy2(src, dst)

    _strip_path(dst)


def _copy_tree(src: Path, dst: Path) -> None:
    def copy_function(s: str, d: str, *, follow_symlinks: bool = True) -> str:
        s_path = Path(s)
        d_path = Path(d)
        if not _needs_update(s_path, d_path):
            return d
        return shutil.copy2(s, d, follow_symlinks=follow_symlinks)

    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        src,
        dst,
        dirs_exist_ok=True,
        symlinks=True,
        copy_function=copy_function,
    )


def _copy_src(src_path: Path, dst_path: Path) -> None:
    if src_path.is_symlink():
        _copy_symlink(src_path, dst_path)
    elif src_path.is_dir():
        if src_path.name == "__pycache__":
            return
        _copy_tree(src_path, dst_path)
    else:
        _copy_file(src_path, dst_path)


def _remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _force_symlink(target: str, link: Path) -> None:
    _remove_path(link)
    os.symlink(target, link)


def _resolve_src(src: str, src_root_dir: str | Path | None) -> Path:
    src_path = Path(src)
    if src_root_dir is not None:
        src_path = Path(src_root_dir) / src_path
    return src_path


def copy_files(
    dst_root_dir: str | Path | None,
    src_dst: Mapping[str, str] | None = None,
    *,
    src_root_dir: str | Path | None = None,
    print_deps: bool = False,
) -> int:
    """Copy a ``{src: dst}`` mapping below ``dst_root_dir``.

    ``src`` is a host path, or a path relative to ``src_root_dir`` when one is
    given. ``dst`` is relative to ``dst_root_dir`` and must name the final
    destination, not a parent directory for directory sources.
    ``src`` may also be a shell-style glob pattern; matched entries are copied
    into ``dst`` under their own names.
    Symlinks are preserved rather than dereferenced.
    When ``print_deps`` is true, print the source paths and return.
    """

    if print_deps:
        if src_dst is None:
            raise ValueError("src_dst is required when printing deps")
        for src in src_dst:
            src_path = _resolve_src(src, src_root_dir)
            src_text = os.fspath(src_path)
            if src_path.is_dir():
                if src_path.name == "__pycache__":
                    continue
                for path in sorted(src_path.rglob("*")):
                    if path.is_file() or path.is_symlink():
                        print(path)
            elif glob.has_magic(src_text):
                for match in sorted(glob.glob(src_text, recursive=True)):
                    print(match)
            else:
                print(src_path)
        return 0

    if dst_root_dir is None or src_dst is None:
        raise ValueError("dst_root_dir and src_dst are required when not printing deps")

    root = Path(dst_root_dir)
    for src, dst in src_dst.items():
        src_text = os.fspath(_resolve_src(src, src_root_dir))
        if glob.has_magic(src_text):
            matches = sorted(
                Path(match) for match in glob.glob(src_text, recursive=True)
            )
            if not matches:
                continue
            for match in matches:
                _copy_src(match, root / dst / match.name)
        else:
            _copy_src(Path(src_text), root / dst)

    return len(src_dst)


def copy_rootfs_files(
    dst_root_dir: str | Path | None,
    *,
    print_deps: bool = False,
) -> int:
    files = {
        "rd/_ramdisk0": ".",
        "app/system/*/*.exe": "sbin",
        "app/usr/oksh-7.8-install/bin/oksh": "bin/oksh",
        "app/usr/toybox-0.8.9-install/bin/*": "bin/toybox",
        "rd/tools/rd-make.c": "root/work/rd-make.c",

        "app/usr/sbase-install/bin/*": "usr/bin/sbase",
        "app/usr/ubase-install/bin/*": "usr/bin/ubase",
        "app/example/*/*.exe": "usr/bin/example",
        "app/test/*/*.exe": "usr/bin/test",
        "app/test/musl/*/*.exe": "usr/bin/test",
        "app/usr/ncurses-6.6-install/share/terminfo/a/ansi": "usr/share/terminfo/a/ansi",
        "app/usr/ncurses-6.6-install/share/terminfo/l/linux": "usr/share/terminfo/l/linux",
        "app/usr/ncurses-6.6-install/share/terminfo/v/vt100": "usr/share/terminfo/v/vt100",
        "app/usr/ncurses-6.6-install/share/terminfo/v/vt102": "usr/share/terminfo/v/vt102",
        "app/usr/ncurses-6.6-install/share/terminfo/x/xterm": "usr/share/terminfo/x/xterm",
        "app/usr/ncurses-6.6-install/share/terminfo/s/screen": "usr/share/terminfo/s/screen",
        "app/usr/ncurses-6.6-install/bin/*": "usr/bin",
        "app/usr/ncurses/*.exe": "usr/bin/ncurses",
        "app/usr/ncurses/guide/*.exe": "usr/bin/ncurses/guide",
        "app/usr/vim-9.2.0428-install/bin/*": "usr/bin",
        "app/usr/vim-9.2.0428-install/share/vim": "usr/share/vim",
        "app/usr/tmux-3.8-install/bin/tmux": "usr/bin/tmux",
        "app/usr/nano-9.0-install/bin/*": "usr/bin",
        "app/usr/nano-9.0-install/share/nano": "usr/share/nano",
        "app/usr/less-692-install/bin/*": "usr/bin",
        "app/usr/Python-3.13.13-install-mario/bin/python3": "usr/python/bin/python3",
        "app/usr/Python-3.13.13-install-mario/bin/python3.13": "usr/python/bin/python3.13",
        "app/usr/Python-3.13.13-install-mario/lib/python3.13": "usr/python/lib/python3.13",
    }

    if print_deps:
        return copy_files(
            None,
            files,
            src_root_dir=ROOT,
            print_deps=True,
        )

    if dst_root_dir is None:
        raise ValueError("dst_root_dir is required when not printing deps")

    root = Path(dst_root_dir)
    for rel in (
        "mnt",
        "proc",
        "sys",
    ):
        (root / rel).mkdir(parents=True, exist_ok=True)

    copied = copy_files(root, files, src_root_dir=ROOT)

    _remove_path(root / "usr" / "bin" / "vimtutor")
    _remove_path(root / "usr" / "bin" / "ncursesw_g6-config")

    _force_symlink("vim", root / "usr" / "bin" / "vi")
    _force_symlink("/usr/python/bin/python3", root / "usr" / "bin" / "python")

    return copied
