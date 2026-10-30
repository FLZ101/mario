#!/usr/bin/env python3
r"""Run framebuffer tests through QMP against the GRUB4DOS VBE boot path.

Directives:
  SERIAL: <shell command>
  SERIAL: <<END
  <multiline shell script>
  END
  SERIAL-CHECK: <serial output substring>
  GUI-SEND-KEY: f1
  GUI-TYPE: command
  GUI-CHECK-SCREEN: golden.ppm
  TIMEOUT: seconds
  EXIT:
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

import pexpect
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parent.parent
QEMU = os.environ.get("QEMU", "qemu-system-i386")
HD_IMAGES = [ROOT / "qemu" / f"hd{index}.img" for index in range(1, 4)]
PROMPT = "MARIO_GUI_PROMPT> "
BOOT_PROMPT = "/ # "
PROMPT_PATTERN = re.compile(r"(?m)^\r*MARIO_GUI_PROMPT> ")
DEFAULT_TIMEOUT = 15.0
BOOT_MENU_DELAY = 3.0
KEY_DELAY = 0.05
class GuiTestError(RuntimeError):
    pass


@dataclass
class GuiTest:
    path: Path
    actions: list[tuple[str, object]] = field(default_factory=list)

    def parse(self) -> "GuiTest":
        lines = self.path.read_text(encoding="utf-8").splitlines()
        index = 0
        while index < len(lines):
            raw = lines[index]
            line = raw[1:] if raw.startswith("#") else raw
            if line.startswith("EXIT:"):
                break
            if line.startswith("SERIAL-CHECK:"):
                self.actions.append(("serial-check", line.split(":", 1)[1].strip()))
            elif line.startswith("SERIAL:"):
                value = line.split(":", 1)[1].strip()
                if value.startswith("<<"):
                    delimiter = value[2:].strip()
                    if not delimiter:
                        raise GuiTestError(f"{self.path}: SERIAL heredoc needs a delimiter")
                    script: list[str] = []
                    index += 1
                    while index < len(lines) and lines[index] != delimiter:
                        script.append(lines[index])
                        index += 1
                    if index == len(lines):
                        raise GuiTestError(f"{self.path}: unterminated SERIAL heredoc")
                    self.actions.append(("serial-script", "\n".join(script) + "\n"))
                else:
                    self.actions.append(("serial", value))
            elif line.startswith("GUI-SEND-KEY:"):
                self.actions.append(("send-key", line.split(":", 1)[1].strip()))
            elif line.startswith("GUI-TYPE:"):
                value = line.split(":", 1)[1]
                if value.startswith(" "):
                    value = value[1:]
                if not value:
                    raise GuiTestError(f"{self.path}: empty GUI-TYPE")
                self.actions.append(("type", value))
            elif line.startswith("GUI-CHECK-SCREEN:"):
                golden = (self.path.parent / line.split(":", 1)[1].strip()).resolve()
                if not golden.is_file():
                    raise GuiTestError(f"{self.path}: invalid GUI-CHECK-SCREEN")
                self.actions.append(("screen", golden))
            elif line.startswith("TIMEOUT:"):
                self.actions.append(("timeout", float(line.split(":", 1)[1].strip())))
            elif line.startswith("GUI-"):
                raise GuiTestError(f"{self.path}: unsupported directive {line!r}")
            index += 1
        if not any(action == "screen"
                   for action, _ in self.actions):
            raise GuiTestError(f"{self.path}: every GUI test needs a screenshot assertion")
        return self


class Qmp:
    def __init__(self, path: str):
        self.path = path
        self.socket: socket.socket | None = None
        self.buffer = b""

    def connect(self) -> None:
        deadline = time.monotonic() + DEFAULT_TIMEOUT
        while time.monotonic() < deadline:
            try:
                self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.socket.connect(self.path)
                break
            except OSError:
                if self.socket is not None:
                    self.socket.close()
                    self.socket = None
                time.sleep(0.05)
        if self.socket is None:
            raise GuiTestError(f"QMP socket was not created: {self.path}")
        self._read_message(DEFAULT_TIMEOUT)
        self.command("qmp_capabilities")

    def _read_message(self, timeout: float) -> dict:
        if self.socket is None:
            raise GuiTestError("QMP is not connected")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if line:
                    return json.loads(line)
            self.socket.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                data = self.socket.recv(4096)
            except socket.timeout:
                continue
            if not data:
                raise GuiTestError("QMP disconnected")
            self.buffer += data
        raise GuiTestError("timed out waiting for QMP response")

    def command(self, execute: str, arguments: dict | None = None) -> dict:
        if self.socket is None:
            raise GuiTestError("QMP is not connected")
        request = {"execute": execute}
        if arguments is not None:
            request["arguments"] = arguments
        self.socket.sendall(json.dumps(request).encode() + b"\n")
        while True:
            response = self._read_message(DEFAULT_TIMEOUT)
            if "event" in response:
                continue
            if "error" in response:
                raise GuiTestError(f"QMP {execute}: {response['error']}")
            return response.get("return", {})

    def close(self) -> None:
        if self.socket is not None:
            self.socket.close()
            self.socket = None


def expect_prompt(child: pexpect.spawn, timeout: float, description: str) -> str:
    try:
        child.expect(PROMPT_PATTERN, timeout=timeout)
    except pexpect.exceptions.TIMEOUT as exc:
        raise GuiTestError(f"{description} timed out; last output: {child.before[-500:]!r}") from exc
    return child.before


def qemu_command(qemu: str, qmp_path: str, smp: int) -> list[str]:
    command = [
        qemu, "-m", "256M", "-smp", str(smp), "-boot", "c", "-vga", "std",
        "-display", "none", "-serial", "stdio", "-qmp", f"unix:{qmp_path},server=on,wait=off",
        "-no-reboot", "-no-shutdown", "-snapshot",
    ]
    for index, image in enumerate(HD_IMAGES, 1):
        command += ["-drive", f"id=gui-hd{index},file={image},format=raw,if=none"]
    command += ["-device", "ahci,id=gui-ahci"]
    for index in range(1, 4):
        command += ["-device", f"ide-hd,drive=gui-hd{index},bus=gui-ahci.{index - 1}"]
    return command


def dump_screen(qmp: Qmp) -> Image.Image:
    with tempfile.NamedTemporaryFile(prefix="mario-gui-", suffix=".ppm", delete=False) as stream:
        path = stream.name
    try:
        qmp.command("screendump", {"filename": path})
        with Image.open(path) as image:
            return image.convert("RGB").copy()
    finally:
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass


def wait_for_screen(qmp: Qmp, golden: Path, timeout: float) -> bool:
    with Image.open(golden) as image:
        expected = image.convert("RGB").copy()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = dump_screen(qmp)
        if current.size == expected.size and \
           ImageChops.difference(current, expected).getbbox() is None:
            return True
        time.sleep(0.05)
    return False


def send_key(qmp: Qmp, key: str) -> None:
    qmp.command("send-key", {"keys": [{"type": "qcode", "data": key}]})


def type_text(qmp: Qmp, text: str) -> None:
    plain = {" ": "spc", "'": "apostrophe"}
    shifted = {"|": "backslash", ":": "semicolon"}

    for character in text:
        if "a" <= character <= "z" or "0" <= character <= "9":
            keys = [{"type": "qcode", "data": character}]
        elif character in plain:
            keys = [{"type": "qcode", "data": plain[character]}]
        elif character in shifted:
            keys = [
                {"type": "qcode", "data": "shift"},
                {"type": "qcode", "data": shifted[character]},
            ]
        else:
            raise GuiTestError(f"unsupported GUI-TYPE character: {character!r}")
        qmp.command("send-key", {"keys": keys})
        time.sleep(KEY_DELAY)
    send_key(qmp, "ret")


def run_serial_script(child: pexpect.spawn, script: str, timeout: float,
                      description: str) -> str:
    delimiter = "__MARIO_SERIAL_EOF__"
    if delimiter in script:
        raise GuiTestError(f"{description}: reserved heredoc delimiter")
    child.sendline(f"/bin/oksh <<'{delimiter}'")
    child.send(script)
    child.sendline(delimiter)
    return expect_prompt(child, timeout, description)


def run_test(test: GuiTest, qemu: str, index: int, smp: int) -> list[str]:
    qmp_path = f"/tmp/mario-gui-qmp-{os.getpid()}-{index}.sock"
    try:
        os.unlink(qmp_path)
    except FileNotFoundError:
        pass
    child = pexpect.spawn(qemu_command(qemu, qmp_path, smp)[0], qemu_command(qemu, qmp_path, smp)[1:], encoding="latin-1", timeout=DEFAULT_TIMEOUT)
    qmp = Qmp(qmp_path)
    failures: list[str] = []
    timeout = DEFAULT_TIMEOUT
    try:
        time.sleep(BOOT_MENU_DELAY)
        child.expect(BOOT_PROMPT, timeout=30)
        child.sendline(f"PS1='{PROMPT}'")
        expect_prompt(child, 10, "set GUI prompt")
        qmp.connect()
        last_serial = ""
        for action, value in test.actions:
            if action == "serial":
                child.sendline(value)
                last_serial = expect_prompt(child, timeout, f"SERIAL {value!r}")
            elif action == "serial-script":
                last_serial = run_serial_script(child, value, timeout, "SERIAL heredoc")
            elif action == "serial-check":
                if value not in last_serial:
                    failures.append(f"SERIAL-CHECK: {value!r}")
            elif action == "send-key":
                send_key(qmp, value)
            elif action == "type":
                type_text(qmp, value)
            elif action == "screen":
                if not wait_for_screen(qmp, value, timeout):
                    failures.append(f"GUI-CHECK-SCREEN: {value}")
            elif action == "timeout":
                timeout = value
    finally:
        try:
            child.sendline("halt")
        finally:
            child.close(force=True)
            qmp.close()
            try:
                os.unlink(qmp_path)
            except FileNotFoundError:
                pass
    return failures


def discover(paths: list[str]) -> list[Path]:
    tests: list[Path] = []
    for value in paths:
        path = Path(value)
        if path.is_dir():
            tests += path.rglob("*.test")
        elif path.is_file():
            tests.append(path)
        else:
            raise GuiTestError(f"path does not exist: {path}")
    return sorted({test.resolve() for test in tests})


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+", help="GUI test files or directories")
    parser.add_argument("--test", help="run a single named test")
    parser.add_argument("--smp", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--qemu", default=QEMU)
    args = parser.parse_args()
    if args.smp < 1 or args.jobs < 1:
        parser.error("--smp and --jobs must be positive")
    missing = [path for path in HD_IMAGES if not path.exists()]
    if missing:
        parser.error("missing disk images: " + ", ".join(str(path) for path in missing))
    tests = discover(args.paths)
    if args.test:
        tests = [test for test in tests if test.name == Path(args.test).name]
    if not tests:
        parser.error("no GUI tests found")

    parsed = [GuiTest(path).parse() for path in tests]
    failed = False
    def execute(item: tuple[int, GuiTest]):
        index, test = item
        started = time.monotonic()
        try:
            failures = run_test(test, args.qemu, index, args.smp)
            return test, time.monotonic() - started, failures, None
        except Exception as exc:
            return test, time.monotonic() - started, [], exc
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for test, elapsed, failures, error in (future.result() for future in as_completed([pool.submit(execute, item) for item in enumerate(parsed)])):
            name = test.path.relative_to(ROOT)
            if error is not None:
                print(f"FAIL: {name} [{elapsed:.2f}s]\n{error}", flush=True)
                failed = True
            elif failures:
                print(f"FAIL: {name} [{elapsed:.2f}s]", flush=True)
                for failure in failures:
                    print(failure, flush=True)
                failed = True
            else:
                print(f"PASS: {name} [{elapsed:.2f}s]", flush=True)
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
