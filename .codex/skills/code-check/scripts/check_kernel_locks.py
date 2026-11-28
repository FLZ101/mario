#!/usr/bin/env python3
"""Scan kernel C for unsafe-under-spinlock operations and lock-order inversions.

All patterns are loaded from a JSON rules file.  The default rules file lives
next to this script at ../config/default.json, so the scanner itself is
codebase-independent.  Projects can add their own lock names, lock/unlock
patterns, forbidden pairs, and unsafe-operation checks.
"""

import argparse
import json
import re
import sys
from pathlib import Path


DEFAULT_CONFIG = Path(__file__).resolve().parent.parent / "config" / "default.json"


def load_rules(path):
    data = json.loads(path.read_text())

    unsafe_checks = []
    for item in data.get("unsafe_checks", []):
        patterns = item.get("patterns", [item.get("pattern")])
        message = item.get("message", "unsafe operation inside spinlock region")
        for pattern in patterns:
            unsafe_checks.append((re.compile(pattern), message))

    generic_lock = re.compile(data["generic_lock_pattern"])
    generic_unlock = re.compile(data["generic_unlock_pattern"])

    locks = []
    for item in data.get("locks", []):
        locks.append(
            {
                "name": item["name"],
                "lock": re.compile(item["lock_pattern"]),
                "unlock": re.compile(item["unlock_pattern"]),
            }
        )

    lock_by_name = {lock["name"]: lock for lock in locks}
    forbidden_pairs = []
    for item in data.get("forbidden_pairs", []):
        before = item["before"]
        after = item["after"]
        if before not in lock_by_name or after not in lock_by_name:
            raise ValueError(f"unknown lock in forbidden_pairs: {before} -> {after}")
        forbidden_pairs.append((before, after))

    return {
        "unsafe_checks": unsafe_checks,
        "generic_lock": generic_lock,
        "generic_unlock": generic_unlock,
        "locks": locks,
        "forbidden_pairs": forbidden_pairs,
    }


def scan_file(path, rules):
    try:
        lines = path.read_text().splitlines()
    except UnicodeDecodeError:
        return []

    findings = []
    generic_depth = 0
    lock_depths = {lock["name"]: 0 for lock in rules["locks"]}
    lock_by_name = {lock["name"]: lock for lock in rules["locks"]}

    for number, line in enumerate(lines, 1):
        spinlock_held = generic_depth > 0 or any(
            depth > 0 for depth in lock_depths.values()
        )

        for before, after in rules["forbidden_pairs"]:
            if lock_depths[before] > 0 and lock_by_name[after]["lock"].search(line):
                findings.append((path, number, f"{before} -> {after} inversion"))

        if spinlock_held:
            for pattern, message in rules["unsafe_checks"]:
                if pattern.search(line):
                    findings.append((path, number, message))

        generic_depth += len(rules["generic_lock"].findall(line))
        generic_depth -= len(rules["generic_unlock"].findall(line))

        for lock in rules["locks"]:
            lock_depths[lock["name"]] += 1 if lock["lock"].search(line) else 0
            lock_depths[lock["name"]] -= 1 if lock["unlock"].search(line) else 0

        generic_depth = max(generic_depth, 0)
        for name in lock_depths:
            lock_depths[name] = max(lock_depths[name], 0)

    return findings


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "path",
        nargs="?",
        default="src",
        help="file or directory to scan (default: src)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG,
        help=f"JSON rules file (default: {DEFAULT_CONFIG})",
    )
    args = parser.parse_args()

    rules = load_rules(args.config)
    root = Path(args.path)
    paths = [root] if root.is_file() else sorted(root.rglob("*.c"))

    found = False
    for path in paths:
        for file_path, line, message in scan_file(path, rules):
            print(f"{file_path}:{line}: {message}")
            found = True

    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
