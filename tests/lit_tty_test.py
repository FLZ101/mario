#!/usr/bin/env python3
r"""Run Mario full-screen terminal tests through a host tmux pane.

Test files use a small lit-style directive set:

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
    EXIT:

QEMU exposes serial0 through a Unix socket.  A dedicated tmux server runs the
local tty_serial_bridge.py client in an 80x25 pane.  This makes tmux's rendered
pane grid the visual/text oracle instead of a VGA screendump.
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

ROOT = Path(__file__).resolve().parent.parent
QEMU = os.environ.get("QEMU", "qemu-system-i386")
KERNEL = ROOT / "src/kernel/kernel.exe"
INITRD = ROOT / "rd/rdx"
HD_IMAGES = [ROOT / f"qemu/hd{index}.img" for index in range(1, 4)]
BRIDGE = ROOT / "tests/tty_serial_bridge.py"

DEFAULT_TIMEOUT = 30.0
PANEL_WIDTH = 80
PANEL_HEIGHT = 25
PROMPT = "MARIO_TTY_PROMPT>"


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
    requires_smp: tuple | None = None
    actions: list[tuple[str, object]] = field(default_factory=list)

    def parse(self) -> "TtyLitTest":
        with self.path.open(encoding="utf-8") as source:
            for raw_line in source:
                line = raw_line.rstrip("\n")
                body = line[1:] if line.startswith("#") else line
                if body.startswith("EXIT:"):
                    break
                if body.startswith("TTY-SEND-RAW:"):
                    self.actions.append(("send_raw", decode_raw(body.split(":", 1)[1].strip())))
                elif body.startswith("TTY-SEND-KEY:"):
                    self.actions.append(("send_key", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-SEND:"):
                    self.actions.append(("send", body.split(":", 1)[1].strip()))
                elif body.startswith("TTY-CHECK-NOT-REGEX:"):
                    self.actions.append(("check_not_regex", body.split(":", 1)[1].strip()))
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
                    self.requires_smp = parse_smp_requirement(body.split(":", 1)[1].strip())
        return self


class TmuxSerialSession:
    def __init__(self, qemu: str, smp: int, index: int):
        self.qemu = qemu
        self.smp = smp
        self.index = index
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

    def start(self) -> None:
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
        ]
        for index, image in enumerate(HD_IMAGES, start=1):
            command += ["-drive", f"id=mario-tty-hd{index},file={image},format=raw,if=none"]
        command += ["-device", "ahci,id=mario-tty-ahci"]
        for index in range(1, 4):
            command += ["-device", f"ide-hd,drive=mario-tty-hd{index},bus=mario-tty-ahci.{index - 1}"]

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
                raise TtyTestError(f"QEMU did not create its serial socket\n{self.qemu_diagnostics()}")
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
        self._wait(lambda panel: "/ #" in panel, DEFAULT_TIMEOUT, "boot prompt")
        self.send("export TERM=screen; PS1='MARIO_TTY_PROMPT> '; clear")
        self._wait(lambda panel: PROMPT in panel, 10, "tty test prompt")

    def send(self, text: str) -> None:
        self._tmux("send-keys", "-t", self.tmux_target, "-l", text)
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
            panel = self._tmux("capture-pane", "-p", "-t", self.tmux_target, capture=True)
        except subprocess.CalledProcessError:
            return ""
        return normalize_panel(panel)

    def wait_contains(self, needle: str, timeout: float) -> str:
        return self._wait(lambda panel: needle in panel, timeout, repr(needle))

    def wait_not_contains(self, needle: str, timeout: float) -> str:
        return self._wait(lambda panel: needle not in panel, timeout, f"absence of {needle!r}")

    def wait_regex(self, pattern: str, timeout: float, want_match: bool) -> str:
        try:
            expression = re.compile(pattern)
        except re.error as error:
            raise TtyTestError(f"invalid regex {pattern!r}: {error}") from error
        description = f"regex {pattern!r}" if want_match else f"absence of regex {pattern!r}"
        return self._wait(lambda panel: bool(expression.search(panel)) == want_match, timeout, description)

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


def run_test(test: TtyLitTest, qemu: str, smp: int, index: int):
    started = time.monotonic()
    session = TmuxSerialSession(qemu, smp, index)
    timeout = DEFAULT_TIMEOUT
    failures: list[str] = []
    report_panel = ""
    try:
        session.start()
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
                resolve_test_path(test.path, str(value)).write_text(session.capture_panel(), encoding="utf-8")
            elif action == "check_panel":
                expected_path = resolve_test_path(test.path, str(value))
                expected = normalize_panel(expected_path.read_text(encoding="utf-8"))
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
            tests.extend(TtyLitTest(candidate).parse() for candidate in sorted(path.rglob("*.test")))
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
        if test.requires_smp is not None and not smp_requirement_matches(test.requires_smp, args.smp):
            skipped.append((test, f"requires SMP {test.requires_smp[2]}"))
        else:
            runnable.append(test)

    for test, reason in skipped:
        print(f"SKIP: {test.path.relative_to(ROOT)} ({reason})")

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
