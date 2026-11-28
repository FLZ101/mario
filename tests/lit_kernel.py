"""Shared kernel-artifact handling for lit integration runners."""

from __future__ import annotations

import fcntl
import hashlib
import os
import subprocess
import tempfile
import threading
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
#   4. When a runner executes multiple REBUILD-KERNEL fixtures in one batch
#      (after all ordinary fixtures finish), ``rebuild_kernel_batch`` holds the
#      writer gate and exclusive artifact lock across the batch and restores
#      the ordinary kernel/image once at batch exit instead of after every
#      individual self-test.
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

# Thread-local state tracking whether the calling thread already owns an active
# rebuild_kernel_batch() lock and whether any selftest build has replaced the
# normal kernel artifacts during that batch.
_BATCH_STATE = threading.local()


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


def _gate_lock_path() -> Path:
    # Separate turnstile lockfile used to prevent Linux flock(LOCK_SH) reader
    # preference from starving a waiting LOCK_EX REBUILD-KERNEL writer.
    identity = hashlib.sha256(str(ROOT).encode()).hexdigest()[:16]
    return (
        Path(tempfile.gettempdir())
        / f"mario-kernel-artifacts-{os.getuid()}-{identity}.gate.lock"
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
def rebuild_kernel_batch(
    disk_image_target: str | None = None,
) -> Generator[None, None, None]:
    """Hold exclusive artifact access across a batch of REBUILD-KERNEL tests.

    Holding the writer gate first prevents new ``LOCK_SH`` readers in other
    processes from starving this batch while active readers drain.  Normal
    kernel and disk-image artifacts are restored once when the batch exits if
    at least one self-test build started.
    """

    if getattr(_BATCH_STATE, "active", False):
        yield
        return

    with _gate_lock_path().open("a+") as gate_file, _lock_path().open(
        "a+"
    ) as lock_file:
        # Block new shared-lock readers at the gate first, then wait for any
        # already-running QEMU guests to release LOCK_SH on the artifact lock.
        fcntl.flock(gate_file.fileno(), fcntl.LOCK_EX)
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            _BATCH_STATE.active = True
            _BATCH_STATE.dirty = False
            try:
                yield
            finally:
                dirty = getattr(_BATCH_STATE, "dirty", False)
                _BATCH_STATE.active = False
                _BATCH_STATE.dirty = False
                try:
                    # Restore the ordinary kernel and disk image once at the
                    # end of the batch before allowing waiting readers in.
                    if dirty:
                        _restore_kernel(disk_image_target)
                finally:
                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
        finally:
            fcntl.flock(gate_file.fileno(), fcntl.LOCK_UN)


@contextmanager
def kernel_artifacts(
    rebuild_target: str | None, disk_image_target: str | None = None
) -> Generator[None, None, None]:
    """Protect running QEMU instances from builds that replace shared artifacts.

    Ordinary tests hold a shared lock for their complete QEMU lifetime.  A
    ``REBUILD-KERNEL`` test takes the exclusive lock, builds its selected test
    kernel, and always restores the ordinary kernel before releasing it.
    """

    if rebuild_target and getattr(_BATCH_STATE, "active", False):
        # Mark artifacts dirty before invoking make, since selftest targets
        # depend on clean-kernel and remove the ordinary kernel immediately.
        _BATCH_STATE.dirty = True
        _build_kernel(rebuild_target, disk_image_target)
        yield
        return

    if rebuild_target:
        with rebuild_kernel_batch(disk_image_target):
            _BATCH_STATE.dirty = True
            _build_kernel(rebuild_target, disk_image_target)
            yield
        return

    # Briefly pass through the writer gate before taking LOCK_SH so an active
    # or queued REBUILD-KERNEL writer in another process is never starved by
    # overlapping shared-lock readers.
    with _gate_lock_path().open("a+") as gate_file, _lock_path().open(
        "a+"
    ) as lock_file:
        fcntl.flock(gate_file.fileno(), fcntl.LOCK_EX)
        try:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_SH)
        finally:
            fcntl.flock(gate_file.fileno(), fcntl.LOCK_UN)
        try:
            yield
        finally:
            fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
