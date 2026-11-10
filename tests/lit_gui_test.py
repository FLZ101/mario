r"""Run framebuffer tests through QMP against the GRUB4DOS VBE boot path.

Directives:
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
  GUI-MOUSE-BUTTON: left,down
  GUI-CHECK-SCREEN: golden.ppm
  GUI-CHECK-RECT: x y width height red green blue
  GUI-CHECK-SCREEN-RECT: golden.ppm,x,y,width,height
  GUI-CHECK-DIFFERENT: golden.ppm
  GUI-CHECK-MATCH: golden.ppm
  GUI-VISUAL-TEST:
  TIMEOUT: seconds
  EXIT:

REBUILD-KERNEL must be the first directive.  It exclusively rebuilds the
kernel and the VBE boot image, then restores both before a following test can
boot.  A rebuilt fixture may either expect a fatal halt or use normal guest/QMP
checks; those assertions determine whether the selected kernel is correct.

Init starts a dormant window manager on framebuffer boots.  Fixtures normally
terminate that service before their actions so they retain exclusive control of
the graphics seat.  INIT-WM: keep opts into the production boot baseline.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import tempfile
import time
from collections.abc import Callable
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
FRAME_COUNTER_PATH = "/tmp/mario-wm.frame"
FRAME_PRESENT_BEGIN = "__MARIO_GUI_FRAME_BEGIN__"
FRAME_PRESENT_END = "__MARIO_GUI_FRAME_END__"
FRAME_MISSING = "__MARIO_GUI_FRAME_MISSING__"


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
) -> tuple[Path, int, int, int, int]:
    """Parse a golden image plus an in-bounds pixel rectangle."""

    fields = [field.strip() for field in value.split(",")]
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
    return asset, x, y, width, height


@dataclass
class GuiTest:
    path: Path
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
                golden = parse_visual_asset(
                    self.path, line.split(":", 1)[1], "GUI-CHECK-SCREEN"
                )
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
    qmp: Qmp, check: tuple[Path, int, int, int, int], timeout: float
) -> bool:
    golden, x, y, width, height = check
    with Image.open(golden) as image:
        expected = image.convert("RGB").crop((x, y, x + width, y + height))
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        current = dump_screen(qmp)
        if (
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
    qmp.command(
        "send-key", {"keys": [{"type": "qcode", "data": key} for key in keys]}
    )


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
    frame_baseline: int | None = None
    visual_failure = False
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
            elif action == "mouse-button":
                if track_frames:
                    frame_baseline = snapshot_guest_frame_counter(child, timeout)
                send_mouse_button(qmp, *value)
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
                if not wait_for_screen(qmp, value, timeout):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-SCREEN timed out after {timeout:.1f}s: {value}"
                    )
            elif action == "rect":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_solid_rect(qmp, value, timeout):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-RECT timed out after {timeout:.1f}s: {value}"
                    )
            elif action == "screen-rect":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen_rect(qmp, value, timeout):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-SCREEN-RECT timed out after {timeout:.1f}s: "
                        f"{value}"
                    )
            elif action == "different":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen_match(qmp, value, timeout, False):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-DIFFERENT timed out after {timeout:.1f}s: {value}"
                    )
            elif action == "match":
                time.sleep(SCREEN_SETTLE_DELAY)
                if not wait_for_screen_match(qmp, value, timeout, True):
                    visual_failure = True
                    failures.append(
                        f"GUI-CHECK-MATCH timed out after {timeout:.1f}s: {value}"
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
