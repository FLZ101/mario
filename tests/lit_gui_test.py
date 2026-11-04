r"""Run framebuffer tests through QMP against the GRUB4DOS VBE boot path.

Directives:
  REBUILD-KERNEL: selftest-kmalloc
  EXPECT-HALT: Assertion 'block->magic == KMALLOC_MAGIC_ALLOC' failed
  SERIAL: <shell command>
  SERIAL: <<END
  <multiline shell script>
  END
  SERIAL-CHECK: <serial output substring>
  GUI-SEND-KEY: f1
  GUI-TYPE: command
  GUI-MOUSE-MOVE: dx,dy
  GUI-MOUSE-BUTTON: left,down
  GUI-CHECK-SCREEN: golden.ppm
  GUI-CHECK-RECT: x y width height red green blue
  TIMEOUT: seconds
  EXIT:

REBUILD-KERNEL must be the first directive.  It exclusively rebuilds the
kernel and the VBE boot image, then restores both before a following test can
boot.  A rebuilt fixture may either expect a fatal halt or use normal guest/QMP
checks; those assertions determine whether the selected kernel is correct.
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

from lit_kernel import kernel_artifacts, validate_rebuild_kernel
from pexpect_utils import make_pexpect_raw, send_line, send_text

ROOT = Path(__file__).resolve().parent.parent
QEMU = os.environ.get("QEMU", "qemu-system-i386")
HD_IMAGES = [ROOT / "qemu" / f"hd{index}.img" for index in range(1, 4)]
PROMPT = "MARIO_GUI_PROMPT> "
BOOT_PROMPT = "/ # "
PROMPT_PATTERN = re.compile(r"(?m)^\r*MARIO_GUI_PROMPT> ")
DEFAULT_TIMEOUT = 60.0
BOOT_MENU_DELAY = 3.0
KEY_DELAY = 0.05
SCREEN_SETTLE_DELAY = 1.0


class GuiTestError(RuntimeError):
    pass


@dataclass
class GuiTest:
    path: Path
    rebuild_kernel: str | None = None
    expect_halt: str | None = None
    actions: list[tuple[str, object]] = field(default_factory=list)

    def parse(self) -> "GuiTest":
        lines = self.path.read_text(encoding="utf-8").splitlines()
        index = 0
        # The disk image is restaged for a rebuild, before the GUI runner can
        # launch QEMU or connect QMP.
        saw_instruction = False
        while index < len(lines):
            raw = lines[index]
            line = raw[1:] if raw.startswith("#") else raw
            if line.startswith("EXIT:"):
                break
            if not line.startswith("REBUILD-KERNEL:") and line.startswith(
                ("SERIAL-CHECK:", "SERIAL:", "GUI-", "TIMEOUT:", "EXPECT-HALT:")
            ):
                saw_instruction = True
            if line.startswith("REBUILD-KERNEL:"):
                if self.rebuild_kernel is not None:
                    raise GuiTestError(f"{self.path}: duplicate REBUILD-KERNEL")
                if saw_instruction:
                    raise GuiTestError(
                        f"{self.path}: REBUILD-KERNEL must be the first directive"
                    )
                self.rebuild_kernel = validate_rebuild_kernel(
                    line.split(":", 1)[1].strip(), self.path
                )
                saw_instruction = True
            elif line.startswith("EXPECT-HALT:"):
                if self.actions:
                    raise GuiTestError(
                        f"{self.path}: EXPECT-HALT must precede test actions"
                    )
                if self.expect_halt is not None:
                    raise GuiTestError(f"{self.path}: duplicate EXPECT-HALT")
                self.expect_halt = line.split(":", 1)[1].strip()
                if not self.expect_halt:
                    raise GuiTestError(f"{self.path}: empty EXPECT-HALT value")
            elif line.startswith("SERIAL-CHECK:"):
                self.actions.append(("serial-check", line.split(":", 1)[1].strip()))
            elif line.startswith("SERIAL:"):
                value = line.split(":", 1)[1].strip()
                if value.startswith("<<"):
                    delimiter = value[2:].strip()
                    if not delimiter:
                        raise GuiTestError(
                            f"{self.path}: SERIAL heredoc needs a delimiter"
                        )
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
            elif line.startswith("GUI-MOUSE-MOVE:"):
                values = line.split(":", 1)[1].strip().split(",")
                if len(values) != 2:
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-MOVE")
                try:
                    move = (int(values[0]), int(values[1]))
                except ValueError as exc:
                    raise GuiTestError(
                        f"{self.path}: invalid GUI-MOUSE-MOVE"
                    ) from exc
                self.actions.append(("mouse-move", move))
            elif line.startswith("GUI-MOUSE-BUTTON:"):
                values = line.split(":", 1)[1].strip().split(",")
                if (
                    len(values) != 2
                    or values[0] not in {"left", "middle", "right"}
                ):
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-BUTTON")
                state = values[1].strip()
                if state not in {"down", "up"}:
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-BUTTON")
                self.actions.append(("mouse-button", (values[0], state == "down")))
            elif line.startswith("GUI-CHECK-SCREEN:"):
                golden = (self.path.parent / line.split(":", 1)[1].strip()).resolve()
                if not golden.is_file():
                    raise GuiTestError(f"{self.path}: invalid GUI-CHECK-SCREEN")
                self.actions.append(("screen", golden))
            elif line.startswith("GUI-CHECK-RECT:"):
                try:
                    values = tuple(
                        int(value)
                        for value in line.split(":", 1)[1].split()
                    )
                except ValueError as exc:
                    raise GuiTestError(
                        f"{self.path}: invalid GUI-CHECK-RECT"
                    ) from exc
                if (
                    len(values) != 7
                    or values[2] <= 0
                    or values[3] <= 0
                    or any(value < 0 or value > 255 for value in values[4:])
                ):
                    raise GuiTestError(f"{self.path}: invalid GUI-CHECK-RECT")
                self.actions.append(("rect", values))
            elif line.startswith("TIMEOUT:"):
                self.actions.append(("timeout", float(line.split(":", 1)[1].strip())))
            elif line.startswith("GUI-"):
                raise GuiTestError(f"{self.path}: unsupported directive {line!r}")
            index += 1
        if self.expect_halt is not None:
            disallowed = {
                "serial",
                "serial-script",
                "send-key",
                "type",
                "mouse-move",
                "mouse-button",
                "screen",
                "rect",
                "timeout",
            }
            if any(action in disallowed for action, _value in self.actions):
                raise GuiTestError(
                    f"{self.path}: EXPECT-HALT tests may only use SERIAL-CHECK"
                )
        return self


class Qmp:
    def __init__(self, path: str):
        self.path = path
        self.socket: socket.socket | None = None
        self.buffer = b""
        self.mouse_buttons = 0

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
        raise GuiTestError(
            f"{description} timed out; last output: {child.before[-500:]!r}"
        ) from exc
    return child.before


def qemu_command(qemu: str, qmp_path: str, smp: int) -> list[str]:
    command = [
        qemu,
        "-m",
        "256M",
        "-smp",
        str(smp),
        "-boot",
        "c",
        "-vga",
        "std",
        "-display",
        "none",
        "-serial",
        "stdio",
        "-qmp",
        f"unix:{qmp_path},server=on,wait=off",
        "-no-reboot",
        "-no-shutdown",
        "-snapshot",
    ]
    for index, image in enumerate(HD_IMAGES, 1):
        command += ["-drive", f"id=gui-hd{index},file={image},format=raw,if=none"]
    command += ["-device", "ahci,id=gui-ahci"]
    for index in range(1, 4):
        command += ["-device", f"ide-hd,drive=gui-hd{index},bus=gui-ahci.{index - 1}"]
    return command


def dump_screen(qmp: Qmp) -> Image.Image:
    with tempfile.NamedTemporaryFile(
        prefix="mario-gui-", suffix=".ppm", delete=False
    ) as stream:
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
        if (
            current.size == expected.size
            and ImageChops.difference(current, expected).getbbox() is None
        ):
            return True
        time.sleep(0.05)
    return False


def wait_for_solid_rect(
    qmp: Qmp, rect: tuple[int, int, int, int, int, int, int], timeout: float
) -> bool:
    x, y, width, height, red, green, blue = rect
    expected = (red, green, blue)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        image = dump_screen(qmp)
        if (
            x >= 0
            and y >= 0
            and x + width <= image.width
            and y + height <= image.height
            and all(
                pixel == expected
                for pixel in image.crop(
                    (x, y, x + width, y + height)
                ).get_flattened_data()
            )
        ):
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


def send_mouse_move(qmp: Qmp, dx: int, dy: int) -> None:
    # HMP targets QEMU's active PS/2 mouse, unlike unstable frontend device IDs.
    qmp.command(
        "human-monitor-command", {"command-line": f"mouse_move {dx} {dy}"}
    )


def send_mouse_button(qmp: Qmp, button: str, down: bool) -> None:
    masks = {"left": 1, "middle": 2, "right": 4}

    if down:
        qmp.mouse_buttons |= masks[button]
    else:
        qmp.mouse_buttons &= ~masks[button]
    qmp.command(
        "human-monitor-command",
        {"command-line": f"mouse_button {qmp.mouse_buttons}"},
    )


def run_serial_script(
    child: pexpect.spawn, script: str, timeout: float, description: str
) -> str:
    delimiter = "__MARIO_SERIAL_EOF__"
    if delimiter in script:
        raise GuiTestError(f"{description}: reserved heredoc delimiter")
    send_line(child, f"/bin/oksh <<'{delimiter}'")
    send_text(child, script)
    send_line(child, delimiter)
    return expect_prompt(child, timeout, description)


def run_test(test: GuiTest, qemu: str, index: int, smp: int) -> list[str]:
    with kernel_artifacts(test.rebuild_kernel, "disk-image-vbe_1024x768"):
        return _run_test_locked(test, qemu, index, smp)


def _run_test_locked(test: GuiTest, qemu: str, index: int, smp: int) -> list[str]:
    qmp_path = f"/tmp/mario-gui-qmp-{os.getpid()}-{index}.sock"
    try:
        os.unlink(qmp_path)
    except FileNotFoundError:
        pass
    child = pexpect.spawn(
        qemu_command(qemu, qmp_path, smp)[0],
        qemu_command(qemu, qmp_path, smp)[1:],
        encoding="latin-1",
        timeout=DEFAULT_TIMEOUT,
    )
    make_pexpect_raw(child)
    qmp = Qmp(qmp_path)
    failures: list[str] = []
    timeout = DEFAULT_TIMEOUT
    try:
        time.sleep(BOOT_MENU_DELAY)
        if test.expect_halt is not None:
            output = []
            child.expect("=== MARIO FATAL HANG ===", timeout=30)
            output.append(child.before + child.after)
            child.expect(re.escape(test.expect_halt), timeout=10)
            output.append(child.before + child.after)
            child.expect("halted", timeout=10)
            output.append(child.before + child.after)
            last_serial = "".join(output)
        else:
            child.expect(BOOT_PROMPT, timeout=30)
            send_line(child, f"PS1='{PROMPT}'")
            expect_prompt(child, 10, "set GUI prompt")
            qmp.connect()
            last_serial = ""
        for action, value in test.actions:
            if action == "serial":
                send_line(child, value)
                last_serial = expect_prompt(child, timeout, f"SERIAL {value!r}")
            elif action == "serial-script":
                last_serial = run_serial_script(child, value, timeout, "SERIAL heredoc")
            elif action == "serial-check":
                if value not in last_serial:
                    failures.append(
                        f"SERIAL-CHECK: {value!r}; last output: {last_serial!r}"
                    )
            elif action == "send-key":
                send_key(qmp, value)
            elif action == "type":
                type_text(qmp, value)
            elif action == "mouse-move":
                send_mouse_move(qmp, *value)
            elif action == "mouse-button":
                send_mouse_button(qmp, *value)
            elif action == "screen":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen(qmp, value, timeout):
                    failures.append(
                        f"GUI-CHECK-SCREEN timed out after {timeout:.1f}s: {value}"
                    )
            elif action == "rect":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_solid_rect(qmp, value, timeout):
                    failures.append(
                        f"GUI-CHECK-RECT timed out after {timeout:.1f}s: {value}"
                    )
            elif action == "timeout":
                timeout = value
    finally:
        try:
            if test.expect_halt is None:
                send_line(child, "halt")
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
        for test, elapsed, failures, error in (
            future.result()
            for future in as_completed(
                [pool.submit(execute, item) for item in enumerate(parsed)]
            )
        ):
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
