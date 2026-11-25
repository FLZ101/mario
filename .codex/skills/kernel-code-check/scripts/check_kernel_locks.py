#!/usr/bin/env python3
"""Flag likely user-memory accesses inside Mario spinlock regions.

This is intentionally narrow: it only reports patterns that are unsafe while a
spinlock is held. It does not model nested wait helpers that release their own
lock, so review blocking calls manually with the SKILL.md guidance.
"""

import argparse
import re
import sys
from pathlib import Path

LOCK_RE = re.compile(r"\b(?:spin_lock_irqsave|spin_lock|ACQUIRE_LOCK)\s*\(")
UNLOCK_RE = re.compile(r"\b(?:spin_unlock_irqrestore|spin_unlock|RELEASE_LOCK)\s*\(")

CHECKS = [
    (
        re.compile(r"\bmemcpy_(?:tofs|fromfs|fs)\s*\("),
        "user-memory copy inside spinlock region",
    ),
    (
        re.compile(r"\b(?:get|put)_fs_long\s*\("),
        "user-memory access inside spinlock region",
    ),
    (
        re.compile(r"\bring_buffer_(?:read|write)\s*\([^;]*,\s*1\s*\)"),
        "ring_buffer user copy (u=1) inside spinlock region",
    ),
]


def scan_file(path: Path) -> list[tuple[Path, int, str]]:
    try:
        lines = path.read_text().splitlines()
    except UnicodeDecodeError:
        return []

    findings = []
    depth = 0

    for number, line in enumerate(lines, 1):
        if depth > 0:
            for pattern, message in CHECKS:
                if pattern.search(line):
                    findings.append((path, number, message))
        depth += len(LOCK_RE.findall(line))
        depth -= len(UNLOCK_RE.findall(line))
        if depth < 0:
            depth = 0

    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "path",
        nargs="?",
        default="src",
        help="file or directory to scan (default: src)",
    )
    args = parser.parse_args()

    root = Path(args.path)
    if root.is_file():
        paths = [root]
    else:
        paths = sorted(root.rglob("*.c"))

    found = False
    for path in paths:
        for file_path, line, message in scan_file(path):
            print(f"{file_path}:{line}: {message}")
            found = True

    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
