"""Shared kernel-artifact handling for lit integration runners."""

from __future__ import annotations

import fcntl
import hashlib
import os
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Generator


ROOT = Path(__file__).resolve().parent.parent
# REBUILD-KERNEL is a test-header directive, not a correctness check for other
# tests.  It must be the first directive so the selected kernel is built before
# that test's runner starts QEMU.
#
# The artifact-lock lifecycle is deliberately asymmetric:
#
#   1. A normal lit test takes a shared lock before it starts QEMU and retains
#      it until QEMU is closed.  Multiple normal tests can therefore run in
#      parallel using the same ordinary kernel and base disk images.
#   2. A REBUILD-KERNEL test takes the exclusive lock, waits for those normal
#      tests to close QEMU, builds the selected self-test kernel, and restages
#      any runner-specific boot image while no normal test can boot it.
#   3. Once the fixture has checked either its expected fatal halt or its
#      ordinary guest behavior, the rebuild test cleans and rebuilds the
#      ordinary kernel (and normal boot image) before it releases the
#      exclusive lock.  A following normal test therefore waits and can only
#      boot restored artifacts.
#
# This prevents cross-test artifact contamination; it does not replace each
# normal test's CHECK/TTY-CHECK/GUI-CHECK assertions.  Guest writes are also
# isolated by each runner's QEMU -snapshot option.  Manual QEMU or make
# invocations outside these runners do not participate in this lock.
REBUILD_KERNEL_TARGETS = frozenset(
    {
        "selftest-kmalloc",
        "selftest-page",
        "selftest-stack",
        "selftest-ubsan",
        "selftest-fb-write-generation",
        "selftest-fb-write-chunks",
        "selftest-fb-transition-race",
        "selftest-fbcon-start-failure",
    }
)


class KernelBuildError(RuntimeError):
    pass


def validate_rebuild_kernel(target: str, test_path: Path | str) -> str:
    if target not in REBUILD_KERNEL_TARGETS:
        supported = ", ".join(sorted(REBUILD_KERNEL_TARGETS))
        raise ValueError(
            f"{test_path}: unsupported REBUILD-KERNEL target {target!r}; "
            f"supported targets: {supported}"
        )
    return target


def _lock_path() -> Path:
    identity = hashlib.sha256(str(ROOT).encode()).hexdigest()[:16]
    return (
        Path(tempfile.gettempdir())
        / f"mario-kernel-artifacts-{os.getuid()}-{identity}.lock"
    )


def _run_make(target: str) -> None:
    completed = subprocess.run(
        ["make", target],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode:
        output = completed.stdout.rstrip()
        detail = f"\n{output}" if output else ""
        raise KernelBuildError(
            f"make {target} failed with exit status {completed.returncode}{detail}"
        )


def _build_kernel(
    target: str, disk_image_target: str | None, announce: bool = True
) -> None:
    if announce:
        print(f"REBUILD-KERNEL: building {target}", flush=True)
    _run_make(target)
    if disk_image_target is not None:
        if announce:
            print(f"REBUILD-KERNEL: staging {disk_image_target}", flush=True)
        _run_make(disk_image_target)


def _restore_kernel(disk_image_target: str | None) -> None:
    print("REBUILD-KERNEL: restoring normal artifacts", flush=True)
    _run_make("clean-kernel")
    _build_kernel("kernel", disk_image_target, announce=False)


@contextmanager
def kernel_artifacts(
    rebuild_target: str | None, disk_image_target: str | None = None
) -> Generator[None, None, None]:
    """Protect running QEMU instances from builds that replace shared artifacts.

    Ordinary tests hold a shared lock for their complete QEMU lifetime.  A
    ``REBUILD-KERNEL`` test takes the exclusive lock, builds its selected test
    kernel, and always restores the ordinary kernel before releasing it.
    """

    with _lock_path().open("a+") as lock_file:
        fcntl.flock(
            lock_file.fileno(), fcntl.LOCK_EX if rebuild_target else fcntl.LOCK_SH
        )
        try:
            if rebuild_target:
                try:
                    _build_kernel(rebuild_target, disk_image_target)
                    yield
                finally:
                    # A failed self-test build may have already removed the
                    # normal kernel, so restore before other tests unlock it.
                    _restore_kernel(disk_image_target)
            else:
                yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
