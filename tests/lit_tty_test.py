r"""Run Mario full-screen terminal tests through a host tmux pane.

Test files use a small lit-style directive set:

    REBUILD-KERNEL: selftest-kmalloc
    EXPECT-HALT: Assertion 'block->magic == KMALLOC_MAGIC_ALLOC' failed
    TTY-SEND: vim /etc/welcome.txt
    TTY-SEND-RAW: gg0
    TTY-SEND-KEY: C-b
    TTY-CHECK: welcome
    TTY-CHECK-NOT: error
    TTY-CHECK-REGEX: [0-9]+ files
    TTY-CHECK-NOT-REGEX: panic
    TTY-DUMP-PANEL: result.txt
    TTY-CHECK-PANEL: expected.txt
    SLEEP: 1
    TIMEOUT: 30
    REQUIRES-SMP: >=4
    SKIP: <reason>
    EXIT:

QEMU exposes serial0 through a Unix socket.  A dedicated tmux server runs the
local tty_serial_bridge.py client in an 80x25 pane.  This makes tmux's rendered
pane grid the visual/text oracle instead of a VGA screendump.

REBUILD-KERNEL must be the first execution directive; SKIP metadata may precede
it. It exclusively rebuilds and
boots a supported self-test kernel, then restores the normal kernel before a
following test can boot.  It isolates artifacts; ordinary TTY-CHECK directives
still determine whether the following test is correct.

SKIP requires a nonempty reason and reports the fixture as skipped without
booting QEMU or rebuilding artifacts. Use --run-skipped to run it explicitly;
REQUIRES-SMP restrictions still apply.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

from lit_kernel import kernel_artifacts, validate_rebuild_kernel
from lit_skip import parse_skip_reason

ROOT = Path(__file__).resolve().parent.parent
QEMU = os.environ.get("QEMU", "qemu-system-i386")
KERNEL = ROOT / "src/kernel/kernel.exe"
INITRD = ROOT / "rd/rdx"
HD_IMAGES = [ROOT / f"qemu/hd{index}.img" for index in range(1, 4)]
BRIDGE = ROOT / "tests/tty_serial_bridge.py"

DEFAULT_TIMEOUT = 60.0
PANEL_WIDTH = 80
PANEL_HEIGHT = 25
PROMPT = "MARIO_TTY_PROMPT>"
TTY_SEND_CHUNK_SIZE = 200
TTY_SEND_CHUNK_DELAY = 0.02


class TtyTestError(RuntimeError):
    pass


class TtyTestTimeout(TtyTestError):
    pass


def parse_smp_requirement(spec: str):
    if not spec:
        return ">=", 2, ">=2"
    match = re.match(r"^\s*(>=|<=|!=|==|>|<|=)?\s*(\d+)\s*$", spec)
    if not match:
        raise ValueError(f"invalid REQUIRES-SMP value: {spec!r}")
    operator = match.group(1) or "=="
    if operator == "=":
        operator = "=="
    return operator, int(match.group(2)), spec.strip()


def smp_requirement_matches(requirement, smp: int) -> bool:
    operator, wanted, _spec = requirement
    if operator == ">=":
        return smp >= wanted
    if operator == "<=":
        return smp <= wanted
    if operator == "!=":
        return smp != wanted
    if operator == ">":
        return smp > wanted
    if operator == "<":
        return smp < wanted
    return smp == wanted


def decode_raw(value: str) -> str:
    return value.encode("utf-8").decode("unicode_escape")


def normalize_panel(text: str) -> str:
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines) + ("\n" if lines else "")


def resolve_test_path(test: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else test.parent / path


@dataclass
class TtyLitTest:
    path: Path
    skip_reason: str | None = None
    requires_smp: tuple | None = None
    rebuild_kernel: str | None = None
    expect_halt: str | None = None
    actions: list[tuple[str, object]] = field(default_factory=list)

    def parse(self) -> "TtyLitTest":
        # Keep a rebuild request ahead of every terminal interaction and QEMU
        # startup-related directive.
        saw_instruction = False
        with self.path.open(encoding="utf-8") as source:
            for raw_line in source:
                line = raw_line.rstrip("\n")
                body = line[1:] if line.startswith("#") else line
                if body.startswith("EXIT:"):
                    break
                if not body.startswith("REBUILD-KERNEL:") and body.startswith(
                    ("TTY-", "SLEEP:", "TIMEOUT:", "REQUIRES-SMP:", "EXPECT-HALT:")
                ):
                    saw_instruction = True
                if body.startswith("SKIP:"):
                    self.skip_reason = parse_skip_reason(
                        self.path, body.split(":", 1)[1], self.skip_reason
                    )
                elif body.startswith("REBUILD-KERNEL:"):
                    if self.rebuild_kernel is not None:
                        raise ValueError(f"{self.path}: duplicate REBUILD-KERNEL")
                    if saw_instruction:
                        raise ValueError(
                            f"{self.path}: REBUILD-KERNEL must be the first directive"
                        )
                    self.rebuild_kernel = validate_rebuild_kernel(
                        body.split(":", 1)[1].strip(), self.path
                    )
                    saw_instruction = True
                elif body.startswith("EXPECT-HALT:"):
                    if self.actions:
                        raise ValueError(
                            f"{self.path}: EXPECT-HALT must precede test actions"
                        )
                    if self.expect_halt is not None:
                        raise ValueError(f"{self.path}: duplicate EXPECT-HALT")
                    self.expect_halt = body.split(":", 1)[1].strip()
                    if not self.expect_halt:
                        raise ValueError(f"{self.path}: empty EXPECT-HALT value")
                elif body.startswith("TTY-SEND-RAW:"):
                    self.actions.append(
                        ("send_raw", decode_raw(body.split(":", 1)[1].strip()))
                    )
                elif body.startswith("TTY-SEND-KEY:"):
                    self.actions.append(("send_key", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-SEND:"):
                    self.actions.append(("send", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-CHECK-NOT-REGEX:"):
                    self.actions.append(
                        ("check_not_regex", body.split(":", 1)[1].strip())
                    )
                elif body.startswith("TTY-CHECK-REGEX:"):
                    self.actions.append(("check_regex", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-CHECK-NOT:"):
                    self.actions.append(("check_not", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-CHECK:"):
                    self.actions.append(("check", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-DUMP-PANEL:"):
                    self.actions.append(("dump_panel", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-CHECK-PANEL:"):
                    self.actions.append(("check_panel", body.split(":", 1)[1].strip()))
                elif body.startswith("SLEEP:"):
                    self.actions.append(("sleep", float(body.split(":", 1)[1].strip())))
                elif body.startswith("TIMEOUT:"):
                    seconds = float(body.split(":", 1)[1].strip())
                    if seconds <= 0:
                        raise ValueError(f"{self.path}: TIMEOUT must be positive")
                    self.actions.append(("timeout", seconds))
                elif body.startswith("REQUIRES-SMP:"):
                    self.requires_smp = parse_smp_requirement(
                        body.split(":", 1)[1].strip()
                    )
        if self.expect_halt is not None:
            disallowed = {"send", "send_raw", "send_key", "sleep", "timeout"}
            if any(action in disallowed for action, _value in self.actions):
                raise ValueError(
                    f"{self.path}: EXPECT-HALT tests may only use TTY-CHECK directives"
                )
        return self


class TmuxSerialSession:
    def __init__(self, qemu: str, smp: int, index: int, *, network=("-nic", "none")):
        self.qemu = qemu
        self.smp = smp
        self.index = index
        self.network = network
        self.tempdir = tempfile.TemporaryDirectory(prefix="mario-tty-")
        self.workdir = Path(self.tempdir.name)
        self.serial_socket = self.workdir / "serial.sock"
        self.qemu_log = self.workdir / "qemu.log"
        token = f"mario-tty-{os.getpid()}-{index}"
        self.tmux_socket = token
        self.tmux_session = token
        self.qemu_process: subprocess.Popen | None = None
        self.qemu_log_file = None

    @property
    def tmux_target(self) -> str:
        return f"{self.tmux_session}:0.0"

    def _tmux(self, *args: str, check: bool = True, capture: bool = False) -> str:
        command = ["tmux", "-L", self.tmux_socket, *args]
        completed = subprocess.run(
            command,
            check=check,
            text=True,
            stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        return completed.stdout if capture else ""

    def _wait(self, predicate, timeout: float, description: str) -> str:
        deadline = time.monotonic() + timeout
        last = ""
        while time.monotonic() < deadline:
            last = self.capture_panel()
            if predicate(last):
                return last
            if self.qemu_process is not None and self.qemu_process.poll() is not None:
                raise TtyTestError(
                    f"QEMU exited while waiting for {description}\n{self.qemu_diagnostics()}"
                )
            time.sleep(0.05)
        raise TtyTestTimeout(
            f"{description} timed out after {timeout:g}s\nLast panel:\n{last[-1000:]}\n"
            f"{self.qemu_diagnostics()}"
        )

    def start(self, expect_halt: str | None = None) -> None:
        if shutil.which("tmux") is None:
            raise TtyTestError("tmux is required for tty integration tests")
        if not BRIDGE.is_file():
            raise TtyTestError(f"missing serial bridge: {BRIDGE}")

        command = [
            self.qemu,
            "-smp",
            str(self.smp),
            "-m",
            "256M",
            "-kernel",
            str(KERNEL),
            "-initrd",
            str(INITRD),
            "-display",
            "none",
            "-serial",
            f"unix:{self.serial_socket},server=on,wait=on",
            "-monitor",
            "none",
            "-no-reboot",
            "-no-shutdown",
            "-snapshot",
            *self.network,
        ]
        for index, image in enumerate(HD_IMAGES, start=1):
            command += [
                "-drive",
                f"id=mario-tty-hd{index},file={image},format=raw,if=none",
            ]
        command += ["-device", "ahci,id=mario-tty-ahci"]
        for index in range(1, 4):
            command += [
                "-device",
                f"ide-hd,drive=mario-tty-hd{index},bus=mario-tty-ahci.{index - 1}",
            ]

        self.qemu_log_file = self.qemu_log.open("w", encoding="utf-8")
        self.qemu_process = subprocess.Popen(
            command,
            cwd=ROOT,
            stdout=self.qemu_log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )

        deadline = time.monotonic() + DEFAULT_TIMEOUT
        while not self.serial_socket.exists():
            if self.qemu_process.poll() is not None:
                raise TtyTestError(
                    f"QEMU did not create its serial socket\n{self.qemu_diagnostics()}"
                )
            if time.monotonic() >= deadline:
                raise TtyTestTimeout(f"timed out waiting for {self.serial_socket}")
            time.sleep(0.02)

        self._tmux(
            "-f",
            "/dev/null",
            "new-session",
            "-d",
            "-x",
            str(PANEL_WIDTH),
            "-y",
            str(PANEL_HEIGHT),
            "-s",
            self.tmux_session,
            sys.executable,
            str(BRIDGE),
            str(self.serial_socket),
        )
        if expect_halt is not None:
            self._wait(
                lambda panel: "=== MARIO FATAL HANG ===" in panel,
                DEFAULT_TIMEOUT,
                "fatal halt",
            )
            self._wait(
                lambda panel: expect_halt in panel,
                10,
                "fatal halt reason",
            )
            self._wait(lambda panel: "halted" in panel, 10, "fatal halt completion")
            return
        self._wait(lambda panel: "/ #" in panel, DEFAULT_TIMEOUT, "boot prompt")
        self.send("export TERM=screen; PS1='MARIO_TTY_PROMPT> '; clear")
        self._wait(lambda panel: PROMPT in panel, 10, "tty test prompt")

    def send(self, text: str) -> None:
        for offset in range(0, len(text), TTY_SEND_CHUNK_SIZE):
            self._tmux(
                "send-keys",
                "-t",
                self.tmux_target,
                "-l",
                text[offset : offset + TTY_SEND_CHUNK_SIZE],
            )
            if offset + TTY_SEND_CHUNK_SIZE < len(text):
                time.sleep(TTY_SEND_CHUNK_DELAY)
        self._tmux("send-keys", "-t", self.tmux_target, "Enter")

    def send_raw(self, text: str) -> None:
        for byte in text.encode("utf-8"):
            self._tmux("send-keys", "-t", self.tmux_target, "-H", f"{byte:02x}")

    def send_key(self, key: str) -> None:
        aliases = {
            "ctrl-b": "C-b",
            "ctrl-c": "C-c",
            "ctrl-d": "C-d",
            "esc": "Escape",
            "escape": "Escape",
            "enter": "Enter",
        }
        self._tmux("send-keys", "-t", self.tmux_target, aliases.get(key.lower(), key))

    def capture_panel(self) -> str:
        try:
            panel = self._tmux(
                "capture-pane", "-p", "-t", self.tmux_target, capture=True
            )
        except subprocess.CalledProcessError:
            return ""
        return normalize_panel(panel)

    def wait_contains(self, needle: str, timeout: float) -> str:
        return self._wait(lambda panel: needle in panel, timeout, repr(needle))

    def wait_not_contains(self, needle: str, timeout: float) -> str:
        return self._wait(
            lambda panel: needle not in panel, timeout, f"absence of {needle!r}"
        )

    def wait_regex(self, pattern: str, timeout: float, want_match: bool) -> str:
        try:
            expression = re.compile(pattern)
        except re.error as error:
            raise TtyTestError(f"invalid regex {pattern!r}: {error}") from error
        description = (
            f"regex {pattern!r}" if want_match else f"absence of regex {pattern!r}"
        )
        return self._wait(
            lambda panel: bool(expression.search(panel)) == want_match,
            timeout,
            description,
        )

    def qemu_diagnostics(self) -> str:
        if not self.qemu_log.exists():
            return ""
        try:
            contents = self.qemu_log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""
        return f"QEMU log:\n{contents[-1000:]}" if contents else ""

    def close(self) -> None:
        try:
            self._tmux("kill-session", "-t", self.tmux_session, check=False)
        finally:
            if self.qemu_process is not None and self.qemu_process.poll() is None:
                self.qemu_process.terminate()
                try:
                    self.qemu_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.qemu_process.kill()
                    self.qemu_process.wait()
            if self.qemu_log_file is not None:
                self.qemu_log_file.close()
            self.tempdir.cleanup()


def run_test(test: TtyLitTest, qemu: str, smp: int, index: int, **session_options):
    started = time.monotonic()
    with kernel_artifacts(test.rebuild_kernel):
        session = TmuxSerialSession(qemu, smp, index, **session_options)
        timeout = DEFAULT_TIMEOUT
        failures: list[str] = []
        report_panel = ""
        try:
            session.start(test.expect_halt)
            for action, value in test.actions:
                if action == "send":
                    session.send(str(value))
                elif action == "send_raw":
                    session.send_raw(str(value))
                elif action == "send_key":
                    session.send_key(str(value))
                elif action == "check":
                    try:
                        session.wait_contains(str(value), timeout)
                    except TtyTestError as error:
                        failures.append(f"TTY-CHECK: {value!r}\n{error}")
                elif action == "check_not":
                    try:
                        session.wait_not_contains(str(value), timeout)
                    except TtyTestError as error:
                        failures.append(f"TTY-CHECK-NOT: {value!r}\n{error}")
                elif action == "check_regex":
                    try:
                        session.wait_regex(str(value), timeout, True)
                    except TtyTestError as error:
                        failures.append(f"TTY-CHECK-REGEX: {value!r}\n{error}")
                elif action == "check_not_regex":
                    try:
                        session.wait_regex(str(value), timeout, False)
                    except TtyTestError as error:
                        failures.append(f"TTY-CHECK-NOT-REGEX: {value!r}\n{error}")
                elif action == "dump_panel":
                    resolve_test_path(test.path, str(value)).write_text(
                        session.capture_panel(), encoding="utf-8"
                    )
                elif action == "check_panel":
                    expected_path = resolve_test_path(test.path, str(value))
                    expected = normalize_panel(
                        expected_path.read_text(encoding="utf-8")
                    )
                    actual = session.capture_panel()
                    if actual != expected:
                        failures.append(
                            f"TTY-CHECK-PANEL: {value!r}\nExpected:\n{expected}\nActual:\n{actual}"
                        )
                elif action == "sleep":
                    time.sleep(float(value))
                elif action == "timeout":
                    timeout = float(value)
            report_panel = session.capture_panel()
        except Exception as error:
            failures.append(str(error))
            report_panel = session.capture_panel()
        finally:
            diagnostics = session.qemu_diagnostics()
            session.close()
    return time.monotonic() - started, failures, report_panel, diagnostics


def discover(paths: list[str]) -> list[TtyLitTest]:
    tests: list[TtyLitTest] = []
    for raw_path in paths:
        path = Path(raw_path).resolve()
        if path.is_dir():
            tests.extend(
                TtyLitTest(candidate).parse()
                for candidate in sorted(path.rglob("*.test"))
            )
        elif path.suffix == ".test":
            tests.append(TtyLitTest(path).parse())
        else:
            raise ValueError(f"not a .test file or directory: {path}")
    return tests


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", default=[str(ROOT / "tests/tty")])
    parser.add_argument("--test", help="run one test path")
    parser.add_argument("--list", action="store_true", help="list discovered tests")
    parser.add_argument("--qemu", default=QEMU)
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--jobs", "-j", type=int, default=1)
    parser.add_argument(
        "--run-skipped", action="store_true", help="run fixtures with an explicit SKIP"
    )
    args = parser.parse_args()

    if args.smp < 1:
        parser.error("--smp must be positive")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.test:
        args.paths = [args.test]

    tests = discover(args.paths)
    if args.list:
        for test in tests:
            print(test.path.relative_to(ROOT))
        return 0

    runnable = []
    skipped = []
    for test in tests:
        if test.skip_reason is not None and not args.run_skipped:
            skipped.append((test, test.skip_reason))
        elif test.requires_smp is not None and not smp_requirement_matches(
            test.requires_smp, args.smp
        ):
            skipped.append((test, f"requires SMP {test.requires_smp[2]}"))
        else:
            runnable.append(test)

    for test, reason in skipped:
        print(f"SKIP: {os.path.relpath(test.path, ROOT)} [{reason}]", flush=True)

    failed = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = {
            executor.submit(run_test, test, args.qemu, args.smp, index): test
            for index, test in enumerate(runnable)
        }
        for future in as_completed(futures):
            test = futures[future]
            name = test.path.relative_to(ROOT)
            try:
                elapsed, failures, panel, diagnostics = future.result()
            except Exception as error:
                failed += 1
                print(f"FAIL: {name} [{time.monotonic():.2f}s]\n{error}")
                continue
            if failures:
                failed += 1
                print(f"FAIL: {name} [{elapsed:.2f}s]")
                for failure in failures:
                    print(failure)
                if panel:
                    print(f"Last panel:\n{panel}")
                if diagnostics:
                    print(diagnostics)
            else:
                print(f"PASS: {name} [{elapsed:.2f}s]")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
