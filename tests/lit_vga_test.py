#!/usr/bin/env python3
r"""A lit-style runner for Mario VGA behavior tests.

Test files use a small subset of lit syntax:

    SERIAL: printf '\033[2J\033[H+---+\n' > /dev/tty1
    VGA-CHECK: +---+
    VGA-CHECK: 3,5,C
    VGA-CHECK-NOT: gone
    VGA-CHECK-REGEX: \+\s*-+
    VGA-SEND: vim /root/test.txt
    VGA-SEND-RAW: ihello
    VGA-DUMP-SCREEN: /tmp/vga-rect.ppm
    VGA-DUMP-SCREEN: /tmp/vga-rect.ppm,1,12
    VGA-CHECK-SCREEN: /tmp/vga-rect.ppm
    VGA-CHECK-SCREEN: /tmp/vga-rect.ppm,1,12
    EXIT:

The serial console is the control channel.  Commands are sent there and the
VGA text buffer is inspected through the QEMU monitor.
Screen paths are relative to the directory containing the test file unless
they are absolute.

Run:

    python3 tests/lit_vga_test.py tests/vga
"""

import argparse
import os
import re
import socket
import sys
import tempfile
import time

try:
    import pexpect
except ImportError:
    print("pexpect is required: pip install pexpect", file=sys.stderr)
    sys.exit(2)

try:
    from PIL import Image, ImageChops
except ImportError:
    print("Pillow is required: pip install Pillow", file=sys.stderr)
    sys.exit(2)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.environ.get("QEMU", "qemu-system-i386")
KERNEL = os.path.join(ROOT, "src", "kernel", "kernel.exe")
INITRD = os.path.join(ROOT, "rd", "rdx")


def _prompt_pattern(literal):
    ansi = r"\x1b(?:\[[0-9;?]*[A-Za-z]|[=>])"
    optional_ansi = r"(?:" + ansi + r")*"
    body = "".join(re.escape(ch) + optional_ansi for ch in literal)
    return r"(?m)^\r*" + optional_ansi + body


BOOT_PROMPT = _prompt_pattern("/ # ")
SERIAL_PROMPT = _prompt_pattern("MARIO_VGA_PROMPT> ")
DEFAULT_TIMEOUT = 15
DEFAULT_VGA_TIMEOUT = 5

VGA_ROWS = 25
VGA_COLS = 80
VGA_TEXT_ADDR = 0xB8000
VGA_BYTES = VGA_ROWS * VGA_COLS * 2
SCREEN_TMPDIR = (
    "/dev/shm" if os.path.isdir("/dev/shm") and os.access("/dev/shm", os.W_OK) else None
)
VGA_SEND_DELAY = 0.02
VGA_SEND_ESC_DELAY = 1.2

VGA_KEYMAP = {
    " ": "spc",
    "/": "slash",
    ".": "dot",
    ":": "shift-semicolon",
    "_": "shift-minus",
    "-": "minus",
    "=": "equal",
    ",": "comma",
    ";": "semicolon",
    "'": "apostrophe",
    "`": "grave_accent",
    "\\": "backslash",
    "\t": "tab",
    "\n": "ret",
    "\r": "ret",
    "!": "shift-1",
    "@": "shift-2",
    "#": "shift-3",
    "$": "shift-4",
    "%": "shift-5",
    "^": "shift-6",
    "&": "shift-7",
    "*": "shift-8",
    "(": "shift-9",
    ")": "shift-0",
    "+": "shift-equal",
    "~": "shift-grave_accent",
    "?": "shift-slash",
    "|": "shift-backslash",
    '"': "shift-apostrophe",
    "<": "shift-comma",
    ">": "shift-dot",
    "\x1b": "esc",
}


class VgaLitTest:
    def __init__(self, path):
        self.path = path
        self.actions = []

    def parse(self):
        with open(self.path, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                body = line[1:] if line.startswith("#") else line
                if body.startswith("EXIT:"):
                    break
                if body.startswith("SERIAL:"):
                    self.actions.append(("serial", _directive_value(body, "SERIAL:")))
                elif body.startswith("VGA-SEND-RAW:"):
                    value = body[len("VGA-SEND-RAW:") :].strip()
                    self.actions.append(
                        ("vga_send_raw", value.encode().decode("unicode_escape"))
                    )
                elif body.startswith("VGA-SEND:"):
                    self.actions.append(("vga_send", body[len("VGA-SEND:") :].strip()))
                elif body.startswith("VGA-CHECK-NOT-REGEX:"):
                    self.actions.append(
                        (
                            "vga_check_not_regex",
                            body[len("VGA-CHECK-NOT-REGEX:") :].strip(),
                        )
                    )
                elif body.startswith("VGA-CHECK-REGEX:"):
                    self.actions.append(
                        ("vga_check_regex", body[len("VGA-CHECK-REGEX:") :].strip())
                    )
                elif body.startswith("VGA-CHECK-NOT:"):
                    self.actions.append(
                        ("vga_check_not", body[len("VGA-CHECK-NOT:") :].strip())
                    )
                elif body.startswith("VGA-CHECK-CELL:"):
                    self.actions.append(
                        (
                            "vga_check_cell",
                            _parse_cell(body[len("VGA-CHECK-CELL:") :]),
                        )
                    )
                elif body.startswith("VGA-CHECK:"):
                    value = body[len("VGA-CHECK:") :].strip()
                    cell = _try_parse_cell(value)
                    if cell is not None:
                        self.actions.append(("vga_check_cell", cell))
                    else:
                        self.actions.append(("vga_check", value))
                elif body.startswith("VGA-DUMP-SCREEN:"):
                    self.actions.append(
                        ("dump-screen", body[len("VGA-DUMP-SCREEN:") :].strip())
                    )
                elif body.startswith("VGA-CHECK-SCREEN:"):
                    self.actions.append(
                        (
                            "vga_check_screen",
                            body[len("VGA-CHECK-SCREEN:") :].strip(),
                        )
                    )
                elif body.startswith("SLEEP:"):
                    self.actions.append(("sleep", float(body[len("SLEEP:") :].strip())))
                elif body.startswith("TIMEOUT:"):
                    seconds = float(body[len("TIMEOUT:") :].strip())
                    if seconds <= 0:
                        raise ValueError(f"{self.path}: TIMEOUT must be positive")
                    self.actions.append(("set-timeout", seconds))
        return self


def _directive_value(line, prefix):
    value = line[len(prefix) :]
    if value.startswith(" "):
        value = value[1:]
    return value


def _try_parse_cell(value):
    parts = value.split(",", 2)
    if len(parts) != 3:
        return None
    row, col, text = parts
    if not row.strip().isdigit() or not col.strip().isdigit():
        return None
    return int(row), int(col), text.strip()


def _parse_cell(value):
    cell = _try_parse_cell(value)
    if cell is None:
        raise ValueError("expected row,col,text after VGA-CHECK-CELL:")
    return cell


class QemuMonitor:
    """Small blocking client for a QEMU monitor unix socket."""

    def __init__(self, path, timeout=8):
        self.path = path
        self.timeout = timeout
        self.sock = None

    def connect(self):
        end = time.time() + self.timeout
        while not os.path.exists(self.path):
            if time.time() >= end:
                raise TimeoutError(f"monitor socket not created: {self.path}")
            time.sleep(0.05)

        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)
        self.sock.recv(4096)  # discard the QEMU greeting

    def command(self, command):
        if self.sock is None:
            self.connect()

        self.sock.sendall((command + "\n").encode("latin-1"))
        data = b""
        end = time.time() + self.timeout
        while b"(qemu)" not in data and time.time() < end:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            except socket.timeout:
                continue

        if b"(qemu)" not in data:
            raise TimeoutError(
                f"QEMU monitor command timed out: {command!r}; got {data!r}"
            )
        return data.decode("latin-1", "replace")

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None


def require_files():
    missing = [p for p in (KERNEL, INITRD) if not os.path.exists(p)]
    if missing:
        print("Missing build outputs:", ", ".join(missing), file=sys.stderr)
        sys.exit(2)


def start_qemu(qemu, monitor_path, timeout=30):
    try:
        os.unlink(monitor_path)
    except FileNotFoundError:
        pass

    cmd = [
        qemu,
        "-m",
        "256M",
        "-kernel",
        KERNEL,
        "-initrd",
        INITRD,
        "-display",
        "none",
        "-serial",
        "stdio",
        "-monitor",
        f"unix:{monitor_path},server=on,wait=off",
        "-no-reboot",
        "-no-shutdown",
    ]
    child = pexpect.spawn(" ".join(cmd), encoding="latin-1", timeout=timeout)
    child.expect(BOOT_PROMPT, timeout=timeout)
    child.sendline("PS1='MARIO_VGA_PROMPT> '")
    child.expect(SERIAL_PROMPT, timeout=10)
    monitor = QemuMonitor(monitor_path)
    monitor.connect()
    return child, monitor


def stop_qemu(child, monitor, monitor_path):
    try:
        child.sendline("halt")
    finally:
        try:
            child.close(force=True)
        finally:
            monitor.close()
            try:
                os.unlink(monitor_path)
            except FileNotFoundError:
                pass


def vga_bytes(monitor):
    out = monitor.command(f"xp /{VGA_BYTES}bx {VGA_TEXT_ADDR:#x}")
    out = "\n".join(line for line in out.splitlines() if re.match(r"^[0-9a-f]+:", line))
    return [int(m.group(1), 16) for m in re.finditer(r"0x([0-9a-f]{2})", out)]


def vga_text(monitor):
    raw = vga_bytes(monitor)
    return "".join(
        chr(raw[index]) if 32 <= raw[index] < 127 else ""
        for index in range(0, len(raw), 2)
    )


def vga_grid(monitor):
    raw = vga_bytes(monitor)
    rows = []
    for row in range(VGA_ROWS):
        chars = []
        for col in range(VGA_COLS):
            byte_index = (row * VGA_COLS + col) * 2
            if byte_index >= len(raw):
                chars.append(" ")
            else:
                ch = raw[byte_index]
                chars.append(chr(ch) if 32 <= ch < 127 else " ")
        rows.append("".join(chars))
    return rows


def _vga_key_name(char):
    if "a" <= char <= "z":
        return char
    if "A" <= char <= "Z":
        return "shift-" + char.lower()
    if "0" <= char <= "9":
        return char
    return VGA_KEYMAP.get(char)


def send_vga_keys(monitor, text):
    for char in text:
        key = _vga_key_name(char)
        if key is None:
            raise ValueError(f"unsupported VGA key: {char!r}")
        monitor.command("sendkey " + key)
        time.sleep(VGA_SEND_DELAY)
        if char == "\x1b":
            # Vim waits for 'ttimeoutlen' after ESC to see whether this is
            # the start of a special key sequence.  Without this pause the
            # next VGA-SEND-RAW key is consumed as part of the escape
            # sequence instead of being seen as a separate key.
            time.sleep(VGA_SEND_ESC_DELAY)


def wait_vga_text(monitor, needle, timeout=DEFAULT_VGA_TIMEOUT):
    text = ""
    end = time.time() + timeout
    while time.time() < end:
        text = vga_text(monitor)
        if needle in text:
            return text, True
        time.sleep(0.1)
    return text, False


def wait_vga_not_text(monitor, needle, timeout=DEFAULT_VGA_TIMEOUT):
    """Wait until a VGA text sample no longer contains needle."""
    text = ""
    end = time.time() + timeout
    while time.time() < end:
        text = vga_text(monitor)
        if needle not in text:
            return text, True
        time.sleep(0.1)
    return text, False


def wait_vga_cell(monitor, row, col, text, timeout=DEFAULT_VGA_TIMEOUT):
    row -= 1
    col -= 1
    end = time.time() + timeout
    while time.time() < end:
        grid = vga_grid(monitor)
        if row < len(grid) and col < len(grid[row]):
            if grid[row][col : col + len(text)] == text:
                return grid, True
        time.sleep(0.1)
    return vga_grid(monitor), False


def resolve_screen_path(path, base_dir):
    return path if os.path.isabs(path) else os.path.join(base_dir, path)


def wait_screen_file(path):
    end = time.time() + 5
    while time.time() < end and (
        not os.path.exists(path) or os.path.getsize(path) == 0
    ):
        time.sleep(0.05)
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        raise RuntimeError(f"failed to dump screen to {path}")


def save_screen_rows(source, target, start_row, end_row):
    with Image.open(source) as image:
        rgb = image.convert("RGB")
        top = (start_row - 1) * 16
        bottom = end_row * 16
        cropped = rgb.crop((0, top, rgb.size[0], bottom))
        cropped.save(target)


def dump_screen(monitor, path, base_dir, crop=None):
    path = resolve_screen_path(path, base_dir)
    if crop is None:
        monitor.command(f"screendump {path}")
        wait_screen_file(path)
        return

    full_fd, full_path = tempfile.mkstemp(
        prefix="mario-vga-dump-screen-", suffix=".ppm", dir=SCREEN_TMPDIR
    )
    os.close(full_fd)
    try:
        monitor.command(f"screendump {full_path}")
        wait_screen_file(full_path)
        save_screen_rows(full_path, path, *crop)
    finally:
        try:
            os.unlink(full_path)
        except FileNotFoundError:
            pass


def screens_equal(left, right):
    with Image.open(left) as left_image, Image.open(right) as right_image:
        left_rgb = left_image.convert("RGB")
        right_rgb = right_image.convert("RGB")
        if left_rgb.size != right_rgb.size:
            return False
        return ImageChops.difference(left_rgb, right_rgb).getbbox() is None


def screens_equal_rows(left, right, start_row, end_row):
    with Image.open(left) as left_image, Image.open(right) as right_image:
        left_rgb = left_image.convert("RGB")
        right_rgb = right_image.convert("RGB")
        if left_rgb.size != right_rgb.size:
            return False
        top = (start_row - 1) * 16
        bottom = end_row * 16
        left_crop = left_rgb.crop((0, top, left_rgb.size[0], bottom))
        right_crop = right_rgb.crop((0, top, right_rgb.size[0], bottom))
        return ImageChops.difference(left_crop, right_crop).getbbox() is None


def parse_screen_check(value):
    parts = value.split(",")
    if len(parts) == 3 and parts[1].strip().isdigit() and parts[2].strip().isdigit():
        return parts[0].strip(), (int(parts[1]), int(parts[2]))
    return value.strip(), None


def run_test(test, qemu, index):
    monitor_path = f"/tmp/mario-vga-lit-{os.getpid()}-{index}.sock"
    child, monitor = start_qemu(qemu, monitor_path)
    failures = []
    timeout = DEFAULT_TIMEOUT
    base_dir = os.path.dirname(test.path)

    try:
        for action, value in test.actions:
            if action == "serial":
                child.sendline(value)
                child.expect(SERIAL_PROMPT, timeout=timeout)
            elif action == "vga_send":
                send_vga_keys(monitor, value + "\n")
            elif action == "vga_send_raw":
                send_vga_keys(monitor, value)
            elif action == "sleep":
                time.sleep(value)
            elif action == "set-timeout":
                timeout = value
            elif action == "vga_check":
                text, found = wait_vga_text(monitor, value)
                if not found:
                    failures.append(f"VGA-CHECK: {value!r}")
                    failures.append(f"  VGA text: {text[-400:]!r}")
            elif action == "vga_check_not":
                text, absent = wait_vga_not_text(monitor, value)
                if not absent:
                    failures.append(f"VGA-CHECK-NOT: {value!r}")
                    failures.append(f"  VGA text: {text[-400:]!r}")
            elif action == "vga_check_regex":
                text, _ = wait_vga_text(monitor, "")
                try:
                    matched = re.search(value, text) is not None
                except re.error as exc:
                    failures.append(
                        f"VGA-CHECK-REGEX: {value!r} (invalid regex: {exc})"
                    )
                    matched = False
                if not matched:
                    failures.append(f"VGA-CHECK-REGEX: {value!r}")
                    failures.append(f"  VGA text: {text[-400:]!r}")
            elif action == "vga_check_not_regex":
                text, _ = wait_vga_text(monitor, "")
                try:
                    matched = re.search(value, text) is not None
                except re.error as exc:
                    failures.append(
                        f"VGA-CHECK-NOT-REGEX: {value!r} (invalid regex: {exc})"
                    )
                    matched = False
                if matched:
                    failures.append(f"VGA-CHECK-NOT-REGEX: {value!r}")
                    failures.append(f"  VGA text: {text[-400:]!r}")
            elif action == "vga_check_cell":
                row, col, text = value
                grid, found = wait_vga_cell(monitor, row, col, text)
                if not found:
                    failures.append(f"VGA-CHECK-CELL: {row},{col},{text!r}")
                    if 1 <= row <= len(grid):
                        failures.append(f"  grid row: {grid[row - 1]!r}")
            elif action == "dump-screen":
                path, crop = parse_screen_check(value)
                dump_screen(monitor, path, base_dir, crop)
            elif action == "vga_check_screen":
                path, crop = parse_screen_check(value)
                expected = resolve_screen_path(path, base_dir)
                current_fd, current_path = tempfile.mkstemp(
                    prefix="mario-vga-check-screen-",
                    suffix=".ppm",
                    dir=SCREEN_TMPDIR,
                )
                os.close(current_fd)
                keep_current = False
                try:
                    dump_screen(monitor, current_path, base_dir)
                    if not os.path.exists(expected):
                        failures.append(f"VGA-CHECK-SCREEN: missing {expected!r}")
                    elif crop is not None:
                        if not screens_equal_rows(current_path, expected, *crop):
                            failures.append(f"VGA-CHECK-SCREEN: {value!r}")
                            failures.append(f"  expected: {expected}")
                            failures.append(f"  actual:   {current_path}")
                            keep_current = True
                    elif not screens_equal(current_path, expected):
                        failures.append(f"VGA-CHECK-SCREEN: {value!r}")
                        failures.append(f"  expected: {expected}")
                        failures.append(f"  actual:   {current_path}")
                        keep_current = True
                finally:
                    if not keep_current:
                        try:
                            os.unlink(current_path)
                        except FileNotFoundError:
                            pass
    finally:
        stop_qemu(child, monitor, monitor_path)

    return failures


def discover_tests(paths, extension):
    test_paths = []
    for path in paths:
        if os.path.isdir(path):
            for root, _dirs, files in os.walk(path):
                for name in files:
                    if name.endswith(extension):
                        test_paths.append(os.path.join(root, name))
        elif os.path.isfile(path):
            test_paths.append(path)
        else:
            raise ValueError(f"path does not exist: {path}")
    return sorted(set(os.path.abspath(p) for p in test_paths))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "paths",
        nargs="+",
        metavar="path",
        help="VGA test files or directories containing .test files",
    )
    parser.add_argument("--qemu", default=QEMU)
    parser.add_argument("--test", help="run only this test file")
    parser.add_argument("--list", action="store_true", help="list discovered tests")
    args = parser.parse_args()

    require_files()
    try:
        test_paths = discover_tests(args.paths, ".test")
    except ValueError as exc:
        parser.error(str(exc))

    if args.test:
        test_paths = [
            p for p in test_paths if os.path.basename(p) == os.path.basename(args.test)
        ]

    if args.list:
        for path in test_paths:
            print(os.path.relpath(path, ROOT))
        sys.exit(0)

    tests = [VgaLitTest(path).parse() for path in test_paths]
    if not tests:
        print(f"No .test files found in: {' '.join(args.paths)}", file=sys.stderr)
        sys.exit(2)

    failed = False
    for index, test in enumerate(tests, 1):
        failures = run_test(test, args.qemu, index)
        name = os.path.relpath(test.path, ROOT)
        if failures:
            failed = True
            print(f"FAIL: {name}")
            for failure in failures:
                print(f"  {failure}")
        else:
            print(f"PASS: {name}")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
