r"""Run framebuffer tests through QMP against the GRUB4DOS VBE boot path.

Directives:
  SKIP: <reason>
  REBUILD-KERNEL: selftest-kmalloc
  INIT-WM: keep
  EXPECT-HALT: Assertion 'block->magic == KMALLOC_MAGIC_ALLOC' failed
  SERIAL: <shell command>
  SERIAL: <<END
  <multiline shell script>
  END
  SERIAL-CHECK: <serial output substring>
  SERIAL-WAIT-FILE: /guest/path
  GUI-WAIT-FRAME:
  GUI-SEND-KEY: f1
  GUI-TYPE: command
  GUI-MOUSE-MOVE: dx,dy
  GUI-MOUSE-POSITION: x,y
  GUI-MOUSE-BUTTON: left,down
  GUI-MOUSE-WHEEL: up
  GUI-POINT: golden.ppm,x,y,width,height[,dx,dy]
  GUI-CLICK: golden.ppm,x,y,width,height[,dx,dy]
  GUI-RIGHT-CLICK: golden.ppm,x,y,width,height[,dx,dy]
  GUI-CHECK-SCREEN: golden.ppm
  GUI-CHECK-RECT: x y width height red green blue
  GUI-CHECK-RECT: anywhere width height red green blue
  GUI-CHECK-SCREEN-RECT: golden.ppm,x,y,width,height
  GUI-CHECK-SCREEN-RECT: golden.ppm,x,y,width,height,anywhere
  GUI-CHECK-DIFFERENT: golden.ppm
  GUI-CHECK-MATCH: golden.ppm
  GUI-VISUAL-TEST:
  TIMEOUT: seconds
  EXIT:

REBUILD-KERNEL must be the first execution directive; SKIP metadata may precede
it. It exclusively rebuilds the
kernel and the VBE boot image, then restores both before a following test can
boot.  A rebuilt fixture may either expect a fatal halt or use normal guest/QMP
checks; those assertions determine whether the selected kernel is correct.

Init starts a dormant window manager on framebuffer boots.  Fixtures normally
terminate that service before their actions so they retain exclusive control of
the graphics seat.  INIT-WM: keep opts into the production boot baseline.

SKIP requires a nonempty reason and reports the fixture as skipped without
booting QEMU or rebuilding artifacts. Use --run-skipped to run it explicitly.

GUI-CHECK-* comparisons retry for at most five seconds after the screen-settle
delay, or less if TIMEOUT is shorter. Image-based pointer actions use the
same retry limit, without the screen-settle delay. Serial commands and
application/frame readiness waits retain TIMEOUT.
The anywhere forms require an exact match anywhere on the guest screen. For
GUI-CHECK-SCREEN-RECT, x and y still select the crop in the reference image.

Boot-prompt timeouts retain the screen, serial output, QMP status and per-CPU
registers under tmp/gui-failures before QEMU is terminated. Diagnostic capture
has a separate five-second total budget; the boot-prompt limit remains 30s.

GUI-MOUSE-POSITION enables the same absolute virtual mouse used by run-gtk;
coordinates are guest pixels. GUI-POINT, GUI-CLICK and GUI-RIGHT-CLICK also
use absolute input, but locate a unique reference crop anywhere on screen.
They target its center or the given offset from the match's top-left.
GUI-CLICK parks the pointer in the top-left corner afterward; GUI-RIGHT-CLICK
leaves it in place for context menus. Use GUI-POINT before gestures or hover.
Absolute input cannot be mixed with GUI-MOUSE-MOVE. Other fixtures retain
relative PS/2 input with VMPort disabled.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import tempfile
import time
from collections.abc import Callable, Iterator
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path

import pexpect
from PIL import Image, ImageChops

from lit_kernel import (
    kernel_artifacts,
    rebuild_kernel_batch,
    validate_rebuild_kernel,
)
from lit_skip import parse_skip_reason
from pexpect_utils import make_pexpect_raw, send_line, send_text

ROOT = Path(__file__).resolve().parent.parent
QEMU = os.environ.get("QEMU", "qemu-system-i386")
HD_IMAGES = [ROOT / "qemu" / f"hd{index}.img" for index in range(1, 4)]
PROMPT = "MARIO_GUI_PROMPT> "
BOOT_PROMPT = "/ # "
PROMPT_PATTERN = re.compile(r"(?m)^\r*MARIO_GUI_PROMPT> ")
DEFAULT_TIMEOUT = 60.0
BOOT_MENU_DELAY = 3.0
BOOT_TIMEOUT = 30.0
BOOT_DIAGNOSTIC_TIMEOUT = 5.0
KEY_DELAY = 0.05
SCREEN_SETTLE_DELAY = 1.0
VISUAL_CHECK_TIMEOUT = 5.0
FRAME_COUNTER_PATH = "/tmp/mario-wm.frame"
FRAME_PRESENT_BEGIN = "__MARIO_GUI_FRAME_BEGIN__"
FRAME_PRESENT_END = "__MARIO_GUI_FRAME_END__"
FRAME_MISSING = "__MARIO_GUI_FRAME_MISSING__"
IMAGE_POINTER_ACTIONS = {
    "GUI-POINT": "point",
    "GUI-CLICK": "click",
    "GUI-RIGHT-CLICK": "right-click",
}
ABSOLUTE_MOUSE_ACTIONS = {"mouse-position", *IMAGE_POINTER_ACTIONS.values()}


class GuiTestError(RuntimeError):
    pass


def parse_visual_asset(test: Path, value: str, directive: str) -> Path:
    """Resolve and validate a fixture-owned screenshot asset."""

    asset = (test.parent / value.strip()).resolve()
    if not value.strip() or not asset.is_file():
        raise GuiTestError(f"{test}: invalid {directive}")
    try:
        with Image.open(asset) as image:
            image.verify()
    except OSError as exc:
        raise GuiTestError(f"{test}: invalid {directive}") from exc
    return asset


def parse_visual_geometry(
    test: Path, value: str, directive: str
) -> tuple[Path, int, int, int, int, bool]:
    """Parse a golden image plus an in-bounds pixel rectangle."""

    fields = [field.strip() for field in value.split(",")]
    anywhere = fields[-1] == "anywhere"
    if anywhere:
        fields.pop()
    if len(fields) != 5:
        raise GuiTestError(f"{test}: invalid {directive}")
    asset = parse_visual_asset(test, fields[0], directive)
    try:
        x, y, width, height = (int(field) for field in fields[1:])
    except ValueError as exc:
        raise GuiTestError(f"{test}: invalid {directive}") from exc
    with Image.open(asset) as image:
        if (
            x < 0
            or y < 0
            or width <= 0
            or height <= 0
            or x + width > image.width
            or y + height > image.height
        ):
            raise GuiTestError(f"{test}: invalid {directive}")
    return asset, x, y, width, height, anywhere


def parse_pointer_target(
    test: Path, value: str, directive: str
) -> tuple[Path, int, int, int, int, int, int]:
    fields = value.split(",")
    if len(fields) not in (5, 7):
        raise GuiTestError(f"{test}: invalid {directive}")
    asset, x, y, width, height, _ = parse_visual_geometry(
        test, ",".join(fields[:5]), directive
    )
    dx, dy = width // 2, height // 2
    if len(fields) == 7:
        try:
            dx, dy = (int(field) for field in fields[5:])
        except ValueError as exc:
            raise GuiTestError(f"{test}: invalid {directive}") from exc
    return asset, x, y, width, height, dx, dy


@dataclass
class GuiTest:
    path: Path
    skip_reason: str | None = None
    rebuild_kernel: str | None = None
    expect_halt: str | None = None
    keep_init_wm: bool = False
    visual_test_markers: int = 0
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
            if not line.startswith(("REBUILD-KERNEL:", "INIT-WM:")) and line.startswith(
                ("SERIAL-CHECK:", "SERIAL:", "GUI-", "TIMEOUT:", "EXPECT-HALT:")
            ):
                saw_instruction = True
            if line.startswith("SKIP:"):
                try:
                    self.skip_reason = parse_skip_reason(
                        self.path, line.split(":", 1)[1], self.skip_reason
                    )
                except ValueError as exc:
                    raise GuiTestError(str(exc)) from exc
            elif line.startswith("REBUILD-KERNEL:"):
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
            elif line.startswith("INIT-WM:"):
                if self.actions or self.keep_init_wm:
                    raise GuiTestError(
                        f"{self.path}: INIT-WM must precede test actions"
                    )
                if line.split(":", 1)[1].strip() != "keep":
                    raise GuiTestError(f"{self.path}: INIT-WM must be 'keep'")
                self.keep_init_wm = True
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
            elif line.startswith("SERIAL-WAIT-FILE:"):
                path = line.split(":", 1)[1].strip()
                if not path.startswith("/") or "\n" in path:
                    raise GuiTestError(f"{self.path}: invalid SERIAL-WAIT-FILE")
                self.actions.append(("serial-wait-file", path))
            elif line.startswith("GUI-WAIT-FRAME:"):
                if line.split(":", 1)[1].strip():
                    raise GuiTestError(f"{self.path}: invalid GUI-WAIT-FRAME")
                self.actions.append(("wait-frame", None))
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
            elif line.startswith("GUI-SEND-KEYS:"):
                keys = tuple(
                    key.strip() for key in line.split(":", 1)[1].strip().split(",")
                )
                if not keys or any(not key.replace("_", "").isalnum() for key in keys):
                    raise GuiTestError(f"{self.path}: invalid GUI-SEND-KEYS")
                self.actions.append(("send-keys", keys))
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
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-MOVE") from exc
                self.actions.append(("mouse-move", move))
            elif line.startswith("GUI-MOUSE-POSITION:"):
                values = line.split(":", 1)[1].strip().split(",")
                try:
                    if len(values) != 2:
                        raise ValueError
                    position = tuple(int(value) for value in values)
                    if any(value < 0 for value in position):
                        raise ValueError
                except ValueError as exc:
                    raise GuiTestError(
                        f"{self.path}: invalid GUI-MOUSE-POSITION"
                    ) from exc
                self.actions.append(("mouse-position", position))
            elif line.startswith("GUI-MOUSE-BUTTON:"):
                values = line.split(":", 1)[1].strip().split(",")
                if len(values) != 2 or values[0] not in {"left", "middle", "right"}:
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-BUTTON")
                state = values[1].strip()
                if state not in {"down", "up"}:
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-BUTTON")
                self.actions.append(("mouse-button", (values[0], state == "down")))
            elif line.startswith("GUI-MOUSE-WHEEL:"):
                direction = line.split(":", 1)[1].strip()
                if direction not in {"up", "down"}:
                    raise GuiTestError(f"{self.path}: invalid GUI-MOUSE-WHEEL")
                self.actions.append(("mouse-wheel", direction))
            elif ":" in line and line.split(":", 1)[0] in IMAGE_POINTER_ACTIONS:
                directive, _, value = line.partition(":")
                self.actions.append(
                    (
                        IMAGE_POINTER_ACTIONS[directive],
                        parse_pointer_target(self.path, value, directive),
                    )
                )
            elif line.startswith("GUI-CHECK-SCREEN:"):
                golden = parse_visual_asset(
                    self.path, line.split(":", 1)[1], "GUI-CHECK-SCREEN"
                )
                self.actions.append(("screen", golden))
            elif line.startswith("GUI-CHECK-RECT:"):
                fields = line.split(":", 1)[1].split()
                anywhere = fields[:1] == ["anywhere"]
                if anywhere:
                    fields = fields[1:]
                try:
                    values = tuple(int(value) for value in fields)
                except ValueError as exc:
                    raise GuiTestError(f"{self.path}: invalid GUI-CHECK-RECT") from exc
                if anywhere:
                    values = (None, None, *values)
                if (
                    len(values) != 7
                    or values[2] <= 0
                    or values[3] <= 0
                    or any(value < 0 or value > 255 for value in values[4:])
                ):
                    raise GuiTestError(f"{self.path}: invalid GUI-CHECK-RECT")
                self.actions.append(("rect", values))
            elif line.startswith("GUI-CHECK-SCREEN-RECT:"):
                self.actions.append(
                    (
                        "screen-rect",
                        parse_visual_geometry(
                            self.path,
                            line.split(":", 1)[1],
                            "GUI-CHECK-SCREEN-RECT",
                        ),
                    )
                )
            elif line.startswith("GUI-CHECK-DIFFERENT:"):
                self.actions.append(
                    (
                        "different",
                        parse_visual_asset(
                            self.path,
                            line.split(":", 1)[1],
                            "GUI-CHECK-DIFFERENT",
                        ),
                    )
                )
            elif line.startswith("GUI-CHECK-MATCH:"):
                self.actions.append(
                    (
                        "match",
                        parse_visual_asset(
                            self.path,
                            line.split(":", 1)[1],
                            "GUI-CHECK-MATCH",
                        ),
                    )
                )
            elif line.startswith("GUI-VISUAL-TEST:"):
                if line.split(":", 1)[1].strip():
                    raise GuiTestError(f"{self.path}: invalid GUI-VISUAL-TEST")
                if self.visual_test_markers:
                    raise GuiTestError(f"{self.path}: duplicate GUI-VISUAL-TEST")
                # A visual declaration describes the whole fixture, not a
                # later sub-step after guest or QMP work has already started.
                if self.actions:
                    raise GuiTestError(
                        f"{self.path}: GUI-VISUAL-TEST must precede test actions"
                    )
                self.visual_test_markers += 1
                self.actions.append(("visual-test", None))
            elif line.startswith("TIMEOUT:"):
                self.actions.append(("timeout", float(line.split(":", 1)[1].strip())))
            elif line.startswith("GUI-"):
                raise GuiTestError(f"{self.path}: unsupported directive {line!r}")
            index += 1
        if self.visual_test_markers:
            screenshot_actions = {
                "screen",
                "rect",
                "screen-rect",
                "different",
                "match",
            }
            # Marked desktop/application fixtures must make a visual claim.
            if not any(action in screenshot_actions for action, _ in self.actions):
                raise GuiTestError(
                    f"{self.path}: GUI-VISUAL-TEST requires a screenshot assertion"
                )
        if self.expect_halt is not None:
            disallowed = {
                "serial",
                "serial-script",
                "serial-wait-file",
                "send-key",
                "send-keys",
                "type",
                "mouse-move",
                "mouse-button",
                "mouse-wheel",
                *ABSOLUTE_MOUSE_ACTIONS,
                "wait-frame",
                "screen",
                "rect",
                "screen-rect",
                "different",
                "match",
                "timeout",
            }
            if any(action in disallowed for action, _value in self.actions):
                raise GuiTestError(
                    f"{self.path}: EXPECT-HALT tests may only use SERIAL-CHECK"
                )
        mouse_actions = {action for action, _ in self.actions}
        if "mouse-move" in mouse_actions and mouse_actions & ABSOLUTE_MOUSE_ACTIONS:
            raise GuiTestError(
                f"{self.path}: GUI-MOUSE-MOVE cannot be mixed with "
                "absolute pointer actions"
            )
        return self


class Qmp:
    def __init__(self, path: str):
        self.path = path
        self.socket: socket.socket | None = None
        self.buffer = b""
        self.mouse_buttons = 0

    def connect(self, timeout: float = DEFAULT_TIMEOUT) -> None:
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            try:
                self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.socket.settimeout(remaining)
                self.socket.connect(self.path)
                break
            except OSError:
                if self.socket is not None:
                    self.socket.close()
                    self.socket = None
                time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
        if self.socket is None:
            raise GuiTestError(f"QMP socket was not created: {self.path}")
        self._read_message(max(0.0, deadline - time.monotonic()))
        self.command("qmp_capabilities", timeout=max(0.0, deadline - time.monotonic()))

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

    def command(
        self,
        execute: str,
        arguments: dict | None = None,
        *,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> dict:
        if self.socket is None:
            raise GuiTestError("QMP is not connected")
        if timeout <= 0:
            raise GuiTestError(f"timed out waiting for QMP {execute} response")
        deadline = time.monotonic() + timeout
        request = {"execute": execute}
        if arguments is not None:
            request["arguments"] = arguments
        self.socket.settimeout(timeout)
        self.socket.sendall(json.dumps(request).encode() + b"\n")
        while (remaining := deadline - time.monotonic()) > 0:
            response = self._read_message(remaining)
            if "event" in response:
                continue
            if "error" in response:
                raise GuiTestError(f"QMP {execute}: {response['error']}")
            return response.get("return", {})
        raise GuiTestError(f"timed out waiting for QMP {execute} response")

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


def qemu_command(
    qemu: str, qmp_path: str, smp: int, *, absolute_mouse: bool = False
) -> list[str]:
    command = [
        qemu,
        "-m",
        "256M",
        "-smp",
        str(smp),
        "-machine",
        f"vmport={'on' if absolute_mouse else 'off'}",
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
        "-nic",
        "none",
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


def retain_visual_failure_diagnostics(
    test: GuiTest,
    qmp: Qmp,
    serial_tail: str,
    index: int,
    failure_root: Path | None = None,
) -> Path:
    """Preserve the final observable GUI state before QEMU cleanup."""

    if failure_root is None:
        failure_root = ROOT / "tmp" / "gui-failures"
    name = re.sub(r"[^A-Za-z0-9_.-]", "-", test.path.stem)
    directory = failure_root / f"{name}-{os.getpid()}-{index}"
    directory.mkdir(parents=True, exist_ok=False)
    errors: list[str] = []
    try:
        qmp.command("screendump", {"filename": str(directory / "actual.ppm")})
    except Exception as exc:
        errors.append(f"screendump: {exc}")
    try:
        status = qmp.command("query-status")
        state = {"query-status": status, "mouse-buttons": qmp.mouse_buttons}
        (directory / "qmp.json").write_text(
            json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except Exception as exc:
        errors.append(f"query-status: {exc}")
    (directory / "serial-tail.txt").write_text(serial_tail[-4000:], encoding="latin-1")
    if errors:
        (directory / "diagnostic-errors.txt").write_text(
            "\n".join(errors) + "\n", encoding="utf-8"
        )
    return directory


def retain_boot_failure_diagnostics(
    test: GuiTest,
    qmp: Qmp,
    serial_tail: str,
    index: int,
    failure_root: Path | None = None,
) -> Path:
    if failure_root is None:
        failure_root = ROOT / "tmp" / "gui-failures"
    name = re.sub(r"[^A-Za-z0-9_.-]", "-", test.path.stem)
    directory = failure_root / f"{name}-boot-{os.getpid()}-{index}"
    directory.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + BOOT_DIAGNOSTIC_TIMEOUT
    state: dict = {}
    errors: list[str] = []

    def remaining() -> float:
        timeout = deadline - time.monotonic()
        if timeout <= 0:
            raise GuiTestError("boot diagnostic deadline exceeded")
        return timeout

    def command(execute: str, arguments: dict | None = None):
        return qmp.command(execute, arguments, timeout=remaining())

    try:
        qmp.connect(timeout=remaining())
    except Exception as exc:
        errors.append(f"QMP connect: {exc}")
    else:
        for execute, arguments in (
            ("query-status", None),
            ("stop", None),
            ("screendump", {"filename": str(directory / "actual.ppm")}),
            ("query-cpus-fast", None),
        ):
            try:
                state[execute] = command(execute, arguments)
            except Exception as exc:
                errors.append(f"{execute}: {exc}")
        state["registers"] = {}
        for cpu in state.get("query-cpus-fast", []):
            cpu_index = cpu["cpu-index"]
            try:
                command("human-monitor-command", {"command-line": f"cpu {cpu_index}"})
                state["registers"][str(cpu_index)] = command(
                    "human-monitor-command", {"command-line": "info registers"}
                )
            except Exception as exc:
                errors.append(f"CPU {cpu_index} registers: {exc}")
    (directory / "qmp.json").write_text(
        json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (directory / "serial-tail.txt").write_text(serial_tail[-4000:], encoding="latin-1")
    if errors:
        (directory / "diagnostic-errors.txt").write_text(
            "\n".join(errors) + "\n", encoding="utf-8"
        )
    return directory


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


def find_screen_rects(
    screen: Image.Image, expected: Image.Image
) -> Iterator[tuple[int, int]]:
    """Find exact RGB crops without wrapping across rows or splitting pixels."""

    width, height = expected.size
    if width > screen.width or height > screen.height:
        return

    stride = screen.width * 3
    row_size = width * 3
    pattern = expected.tobytes()
    rows = tuple(
        pattern[offset : offset + row_size]
        for offset in range(0, len(pattern), row_size)
    )
    anchor = max(range(height), key=lambda index: len(set(rows[index])))
    pixels = screen.tobytes()
    other_rows = tuple(
        (index * stride, rows[index])
        for index in reversed(range(height))
        if index != anchor
    )
    for top in range(0, (screen.height - height + 1) * stride, stride):
        row_start = top + anchor * stride
        row_end = row_start + stride
        match = pixels.find(rows[anchor], row_start, row_end)
        while match != -1:
            offset = match - row_start
            if offset % 3 == 0 and all(
                pixels.startswith(row, top + offset + row_offset)
                for row_offset, row in other_rows
            ):
                yield offset // 3, top // stride
            match = pixels.find(rows[anchor], match + 1, row_end)


def screen_contains_rect(screen: Image.Image, expected: Image.Image) -> bool:
    return next(find_screen_rects(screen, expected), None) is not None


def wait_for_pointer_target(
    qmp: Qmp,
    target: tuple[Path, int, int, int, int, int, int],
    timeout: float,
    directive: str = "GUI-CLICK",
) -> tuple[int, int]:
    golden, x, y, width, height, dx, dy = target
    with Image.open(golden) as image:
        expected = image.convert("RGB").crop((x, y, x + width, y + height))
    deadline = time.monotonic() + timeout
    reason = "no match"
    while time.monotonic() < deadline:
        screen = dump_screen(qmp)
        matches = find_screen_rects(screen, expected)
        match = next(matches, None)
        if match is None:
            reason = "no match"
        elif next(matches, None) is not None:
            reason = "multiple matches"
        else:
            left, top = match
            target_x, target_y = left + dx, top + dy
            if 0 <= target_x < screen.width and 0 <= target_y < screen.height:
                return target_x, target_y
            raise GuiTestError(
                f"{directive} point {(target_x, target_y)} is outside the screen"
            )
        time.sleep(KEY_DELAY)
    raise GuiTestError(
        f"{directive} timed out after {timeout:.1f}s ({reason}): {target}"
    )


def wait_for_solid_rect(
    qmp: Qmp,
    rect: tuple[int | None, int | None, int, int, int, int, int],
    timeout: float,
) -> bool:
    x, y, width, height, red, green, blue = rect
    expected = (red, green, blue)
    pattern = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        image = dump_screen(qmp)
        if x is None or y is None:
            if width <= image.width and height <= image.height:
                if pattern is None:
                    pattern = Image.new("RGB", (width, height), expected)
                if screen_contains_rect(image, pattern):
                    return True
        elif (
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


def wait_for_screen_match(qmp: Qmp, golden: Path, timeout: float, equal: bool) -> bool:
    with Image.open(golden) as image:
        expected = image.convert("RGB").copy()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = dump_screen(qmp)
        matches = (
            current.size == expected.size
            and ImageChops.difference(current, expected).getbbox() is None
        )
        if matches == equal:
            return True
        time.sleep(0.05)
    return False


def wait_for_screen_rect(
    qmp: Qmp, check: tuple[Path, int, int, int, int, bool], timeout: float
) -> bool:
    golden, x, y, width, height, anywhere = check
    with Image.open(golden) as image:
        expected = image.convert("RGB").crop((x, y, x + width, y + height))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = dump_screen(qmp)
        if anywhere:
            if screen_contains_rect(current, expected):
                return True
        elif (
            x + width <= current.width
            and y + height <= current.height
            and ImageChops.difference(
                current.crop((x, y, x + width, y + height)), expected
            ).getbbox()
            is None
        ):
            return True
        time.sleep(0.05)
    return False


def send_key(qmp: Qmp, key: str) -> None:
    qmp.command("send-key", {"keys": [{"type": "qcode", "data": key}]})


def send_keys(qmp: Qmp, keys: tuple[str, ...]) -> None:
    qmp.command("send-key", {"keys": [{"type": "qcode", "data": key} for key in keys]})


def type_text(qmp: Qmp, text: str) -> None:
    plain = {" ": "spc", "'": "apostrophe", ".": "dot", "/": "slash"}
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
    qmp.command("human-monitor-command", {"command-line": f"mouse_move {dx} {dy}"})


def send_mouse_position(qmp: Qmp, x: int, y: int) -> None:
    screen = dump_screen(qmp)
    if not (0 <= x < screen.width and 0 <= y < screen.height):
        raise GuiTestError(
            f"GUI-MOUSE-POSITION {x},{y} is outside {screen.width}x{screen.height}"
        )
    qmp.command(
        "input-send-event",
        {
            "events": [
                {
                    "type": "abs",
                    "data": {"axis": "x", "value": x * 32767 // screen.width},
                },
                {
                    "type": "abs",
                    "data": {"axis": "y", "value": y * 32767 // screen.height},
                },
            ]
        },
    )


def send_mouse_button(qmp: Qmp, button: str, down: bool) -> None:
    masks = {"left": 1, "middle": 2, "right": 4}

    qmp.command(
        "input-send-event",
        {"events": [{"type": "btn", "data": {"button": button, "down": down}}]},
    )
    if down:
        qmp.mouse_buttons |= masks[button]
    else:
        qmp.mouse_buttons &= ~masks[button]


def send_mouse_wheel(qmp: Qmp, direction: str) -> None:
    qmp.command(
        "input-send-event",
        {
            "events": [
                {"type": "btn", "data": {"button": f"wheel-{direction}", "down": down}}
                for down in (True, False)
            ]
        },
    )


def send_click(qmp: Qmp, x: int, y: int, button: str = "left") -> None:
    send_mouse_position(qmp, x, y)
    time.sleep(KEY_DELAY)
    send_mouse_button(qmp, button, True)
    time.sleep(KEY_DELAY)
    send_mouse_button(qmp, button, False)
    if button == "left":
        time.sleep(KEY_DELAY)
        send_mouse_position(qmp, 0, 0)


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


def shell_quote(value: str) -> str:
    """Quote a single guest-shell argument without reading host paths."""

    return "'" + value.replace("'", "'\\\"'\\\"'") + "'"


def wait_for_guest_file(child: pexpect.spawn, path: str, timeout: float) -> str:
    marker = "__MR__"
    deadline = time.monotonic() + timeout
    last_output = ""

    while time.monotonic() < deadline:
        # Print the marker on its own line: command echo must not satisfy the
        # probe, and this keeps long guest paths below the serial line width.
        command = (
            f"test -e {shell_quote(path)} && "
            f"printf '\\n%s\\n' {shell_quote(marker)}"
        )
        send_line(child, command)
        remaining = max(0.01, deadline - time.monotonic())
        last_output = expect_prompt(child, remaining, f"SERIAL-WAIT-FILE {path}")
        if f"\n{marker}\n" in last_output.replace("\r", "\n"):
            return last_output
        time.sleep(0.05)
    raise GuiTestError(
        f"SERIAL-WAIT-FILE timed out after {timeout:.1f}s: {path}; "
        f"last output: {last_output[-500:]!r}"
    )


def parse_frame_counter(value: str) -> int:
    """Return the WM's newline-terminated decimal frame count."""

    value = value.strip()
    if not re.fullmatch(r"[0-9]+", value):
        raise GuiTestError(
            f"GUI-WAIT-FRAME: malformed decimal frame counter: {value!r}"
        )
    return int(value)


def parse_frame_probe(output: str) -> int | None:
    """Decode a delimited serial probe without confusing shell command echo."""

    missing = output.rfind(FRAME_MISSING)
    begin = output.rfind(FRAME_PRESENT_BEGIN)
    end = output.rfind(FRAME_PRESENT_END)
    # The serial console echoes the probe command, which contains every marker.
    # The final emitted marker identifies the result produced by the shell.
    if missing > max(begin, end):
        return None
    if begin < 0 or end < begin:
        raise GuiTestError("GUI-WAIT-FRAME: missing frame probe result")
    return parse_frame_counter(output[begin + len(FRAME_PRESENT_BEGIN) : end])


def read_guest_frame_counter(child: pexpect.spawn, timeout: float) -> int | None:
    """Read the guest-owned frame file only through the serial shell."""

    command = (
        f"printf {shell_quote(FRAME_PRESENT_BEGIN)}; "
        f"if cat {shell_quote(FRAME_COUNTER_PATH)} 2>/dev/null; then "
        f"printf {shell_quote(FRAME_PRESENT_END)}; "
        f"else printf {shell_quote(FRAME_MISSING)}; fi; printf '\\n'"
    )
    send_line(child, command)
    output = expect_prompt(child, timeout, "GUI-WAIT-FRAME frame probe")
    return parse_frame_probe(output)


def wait_for_frame_advance(
    read_counter: Callable[[], int | None],
    baseline: int,
    timeout: float,
    *,
    now: Callable[[], float] = time.monotonic,
    pause: Callable[[float], None] = time.sleep,
) -> int:
    """Wait for a completed composite newer than the pre-input snapshot."""

    deadline = now() + timeout
    last_counter: int | None = None
    while now() < deadline:
        last_counter = read_counter()
        if last_counter is not None and last_counter > baseline:
            return last_counter
        pause(0.05)
    last_value = "missing" if last_counter is None else str(last_counter)
    raise GuiTestError(
        f"GUI-WAIT-FRAME timed out after {timeout:.1f}s: expected a value "
        f"greater than {baseline} at {FRAME_COUNTER_PATH}; last value: {last_value}"
    )


def snapshot_guest_frame_counter(child: pexpect.spawn, timeout: float) -> int:
    """Treat a not-yet-created or removed frame file as frame zero."""

    counter = read_guest_frame_counter(child, timeout)
    return 0 if counter is None else counter


def stop_init_window_manager(child: pexpect.spawn, timeout: float) -> str:
    """Release init's dormant graphics seat for an isolated GUI fixture."""

    return run_serial_script(
        child,
        """while ! test -e /tmp/mario-wm.started; do sleep 1; done
kill -TERM $(pgrep mario-wm)
while test -e /tmp/mario-wm.started; do sleep 1; done
""",
        timeout,
        "stop init window manager",
    )


def run_test(
    test: GuiTest,
    qemu: str,
    index: int,
    smp: int,
    elapsed_out: list[float] | None = None,
) -> list[str]:
    with kernel_artifacts(test.rebuild_kernel, "disk-image-vbe_1024x768"):
        # Time only the QEMU run inside kernel_artifacts() so lock waits and
        # REBUILD-KERNEL build/restore steps are excluded from [elapsed].
        started = time.monotonic()
        try:
            return _run_test_locked(test, qemu, index, smp)
        finally:
            if elapsed_out is not None:
                elapsed_out.append(time.monotonic() - started)


def _run_test_locked(test: GuiTest, qemu: str, index: int, smp: int) -> list[str]:
    qmp_path = f"/tmp/mario-gui-qmp-{os.getpid()}-{index}.sock"
    try:
        os.unlink(qmp_path)
    except FileNotFoundError:
        pass
    command = qemu_command(
        qemu,
        qmp_path,
        smp,
        absolute_mouse=any(
            action in ABSOLUTE_MOUSE_ACTIONS for action, _ in test.actions
        ),
    )
    child = pexpect.spawn(
        command[0],
        command[1:],
        encoding="latin-1",
        timeout=DEFAULT_TIMEOUT,
    )
    make_pexpect_raw(child)
    qmp = Qmp(qmp_path)
    failures: list[str] = []
    timeout = DEFAULT_TIMEOUT
    frame_baseline: int | None = None
    visual_failure = False
    boot_ready = False
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
            try:
                child.expect(BOOT_PROMPT, timeout=BOOT_TIMEOUT)
            except pexpect.exceptions.TIMEOUT as exc:
                diagnostics = retain_boot_failure_diagnostics(
                    test, qmp, child.before, index
                )
                raise GuiTestError(
                    f"boot prompt timed out after {BOOT_TIMEOUT:.1f}s before fixture "
                    f"actions; last serial output: {child.before[-500:]!r}; "
                    f"boot diagnostics retained: {diagnostics}"
                ) from exc
            boot_ready = True
            send_line(child, f"PS1='{PROMPT}'")
            expect_prompt(child, 10, "set GUI prompt")
            qmp.connect()
            if test.keep_init_wm:
                last_serial = ""
            else:
                last_serial = stop_init_window_manager(child, DEFAULT_TIMEOUT)
        # Low-level framebuffer fixtures also inject keys, but only fixtures
        # that opt into GUI-WAIT-FRAME require the WM-owned frame protocol.
        track_frames = any(action == "wait-frame" for action, _ in test.actions)
        for action_index, (action, value) in enumerate(test.actions, 1):
            if action == "visual-test":
                continue
            visual_timeout = min(timeout, VISUAL_CHECK_TIMEOUT)
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
            elif action == "serial-wait-file":
                last_serial = wait_for_guest_file(child, value, timeout)
            elif action == "send-key":
                # Snapshot before input so the wait cannot accept an older frame.
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_key(qmp, value)
            elif action == "send-keys":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_keys(qmp, value)
            elif action == "type":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                type_text(qmp, value)
            elif action == "mouse-move":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_mouse_move(qmp, *value)
            elif action == "mouse-position":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_mouse_position(qmp, *value)
            elif action == "mouse-button":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_mouse_button(qmp, *value)
            elif action == "mouse-wheel":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_mouse_wheel(qmp, value)
            elif action in IMAGE_POINTER_ACTIONS.values():
                try:
                    position = wait_for_pointer_target(
                        qmp, value, visual_timeout, f"GUI-{action.upper()}"
                    )
                except GuiTestError as exc:
                    visual_failure = True
                    failures.append(f"action {action_index}: {exc}")
                    break
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                if action == "point":
                    send_mouse_position(qmp, *position)
                else:
                    button = "right" if action == "right-click" else "left"
                    send_click(qmp, *position, button=button)
            elif action == "wait-frame":
                if frame_baseline is None:
                    raise GuiTestError(
                        "GUI-WAIT-FRAME requires a preceding GUI input action"
                    )
                try:
                    frame_baseline = wait_for_frame_advance(
                        lambda: read_guest_frame_counter(child, timeout),
                        frame_baseline,
                        timeout,
                    )
                except GuiTestError as exc:
                    raise GuiTestError(
                        f"action {action_index} GUI-WAIT-FRAME: {exc}"
                    ) from exc
            elif action == "screen":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen(qmp, value, visual_timeout):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-SCREEN timed out after {visual_timeout:.1f}s: {value}"
                    )
            elif action == "rect":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_solid_rect(qmp, value, visual_timeout):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-RECT timed out after {visual_timeout:.1f}s: {value}"
                    )
            elif action == "screen-rect":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen_rect(qmp, value, visual_timeout):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-SCREEN-RECT timed out after {visual_timeout:.1f}s: "
                        f"{value}"
                    )
            elif action == "different":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen_match(qmp, value, visual_timeout, False):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-DIFFERENT timed out after {visual_timeout:.1f}s: "
                        f"{value}"
                    )
            elif action == "match":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen_match(qmp, value, visual_timeout, True):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-MATCH timed out after {visual_timeout:.1f}s: {value}"
                    )
            elif action == "timeout":
                timeout = value
    finally:
        try:
            if visual_failure:
                diagnostics = retain_visual_failure_diagnostics(
                    test, qmp, last_serial + child.before, index
                )
                failures.append(f"GUI diagnostics retained: {diagnostics}")
            if boot_ready:
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
    parser.add_argument(
        "--run-skipped", action="store_true", help="run fixtures with an explicit SKIP"
    )
    args = parser.parse_args()
    if args.smp < 1 or args.jobs < 1:
        parser.error("--smp and --jobs must be positive")
    tests = discover(args.paths)
    if args.test:
        tests = [test for test in tests if test.name == Path(args.test).name]
    if not tests:
        parser.error("no GUI tests found")

    parsed = [GuiTest(path).parse() for path in tests]
    runnable = []
    for test in parsed:
        if test.skip_reason is not None and not args.run_skipped:
            name = os.path.relpath(test.path, ROOT)
            print(f"SKIP: {name} [{test.skip_reason}]", flush=True)
        else:
            runnable.append(test)
    missing = [path for path in HD_IMAGES if not path.exists()]
    if runnable and missing:
        parser.error("missing disk images: " + ", ".join(str(path) for path in missing))
    failed = False

    def execute(item: tuple[int, GuiTest]):
        index, test = item
        # Measure only guest execution inside kernel_artifacts() so LOCK_EX/LOCK_SH
        # queueing and REBUILD-KERNEL make steps do not inflate fixture time.
        elapsed_box: list[float] = []
        started = time.monotonic()
        try:
            failures = run_test(test, args.qemu, index, args.smp, elapsed_box)
            elapsed = elapsed_box[0] if elapsed_box else time.monotonic() - started
            return test, elapsed, failures, None
        except Exception as exc:
            elapsed = elapsed_box[0] if elapsed_box else time.monotonic() - started
            return test, elapsed, [], exc

    def report_result(
        test: GuiTest,
        elapsed: float,
        failures: list[str],
        error: Exception | None,
    ) -> None:
        nonlocal failed
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

    # Run ordinary shared-kernel fixtures across all worker threads first, then
    # run exclusive REBUILD-KERNEL fixtures in one serial batch at the end so
    # workers never block on LOCK_EX and normal artifacts are restored once.
    indexed = list(enumerate(runnable))
    ordinary = [item for item in indexed if item[1].rebuild_kernel is None]
    rebuilds = [item for item in indexed if item[1].rebuild_kernel is not None]

    if ordinary:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for result in (
                future.result()
                for future in as_completed(
                    [pool.submit(execute, item) for item in ordinary]
                )
            ):
                report_result(*result)

    if rebuilds:
        with rebuild_kernel_batch("disk-image-vbe_1024x768"):
            for item in rebuilds:
                report_result(*execute(item))

    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
