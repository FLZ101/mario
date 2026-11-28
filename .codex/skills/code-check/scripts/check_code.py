#!/usr/bin/env python3
"""Check kernel, userspace GUI/IPC, and guest test invariants.

Covers:
  - src/: spinlock/irqsave user-memory copies, blocking calls, and configured
    lock-order inversions (via check_kernel_locks.py).
  - app/: AF_UNIX surface-upload batching without SO_SNDBUF expansion,
    SO_SNDBUF setsockopt before connect()/socketpair(), and compositor client
    stdio / terminal PTY stdout root-ramdisk hazards.
  - tests/: GUI launcher / background app spawn races before keyboard input
    injection and unmounted /mnt fixture mutations.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import check_kernel_locks

DEFAULT_CONFIG = SCRIPTS_DIR.parent / "config" / "default.json"
APP_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}

COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
BATCH_ENABLE_RE = re.compile(r"(?:\.|\->)batch\s*=\s*(?!(?:0|false)\s*[,;)\]])([^;]+)")
SO_SNDBUF_RE = re.compile(r"\bsetsockopt\s*\([^;]*\bSOL_SOCKET\b[^;]*\bSO_SNDBUF\b")
SOCKET_CALL_RE = re.compile(r"\bsocket\s*\(\s*AF_UNIX\b")
CONNECT_CALL_RE = re.compile(r"\b(?:connect|connect_wm|socketpair|accept)\s*\(")
WM_LAUNCH_FUNC_RE = re.compile(
    r"^static\s+int\s+(wm_start_(?!shell\b)[a-z0-9_]+)\s*\(", re.MULTILINE
)
PTY_STDOUT_ECHO_RE = re.compile(r"\b(?:write|write_full)\s*\(\s*STDOUT_FILENO\b")
MNT_MUTATION_RE = re.compile(
    r"(?:(?:^|[;&|()\s])(?:mkdir|touch|rm|rmdir|cp|mv|ln|truncate)\b[^\n]*/mnt(?:/|\b)"
    r"|(?:>>?|\bof=)\s*['\"]?/mnt(?:/|\b))"
)
MOUNT_MNT_RE = re.compile(r"\b(?:u?mount)\b[^\n]*/mnt(?:/|\b)")
SERIAL_READY_RE = re.compile(
    r"(?:window ready|await_phase|\bkill\b|\.ready\b|\bshown\b|\bloaded\b|\bopened\b)"
)


def load_config(path: Path) -> dict:
    data = json.loads(path.read_text())
    return {
        "kernel_rules": check_kernel_locks.load_rules(path),
        "app_exclude_dirs": [
            re.compile(pattern) for pattern in data.get("app_exclude_dir_patterns", [])
        ],
        "app_unit_test_prefixes": tuple(
            data.get("app_unit_test_prefixes", ["app/test/", "app/test-cxx/"])
        ),
        "gui_test_apps": tuple(data.get("gui_test_apps", [])),
        "gui_launcher_assets": tuple(data.get("gui_launcher_assets", [])),
        "gui_test_vt_keys": set(data.get("gui_test_vt_keys", [])),
    }


def normalize_relpath(path: Path, repo_root: Path | None) -> str:
    if repo_root is not None:
        try:
            return path.resolve().relative_to(repo_root.resolve()).as_posix()
        except ValueError:
            pass
    return path.as_posix()


def strip_c_comments_preserving_lines(text: str) -> str:
    """Replace C/C++ comments with spaces while preserving newline counts."""
    return COMMENT_RE.sub(
        lambda match: "".join("\n" if ch == "\n" else " " for ch in match.group(0)),
        text,
    )


def line_number_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def iter_brace_blocks(text: str, start_brace: int) -> str:
    """Return the balanced `{ ... }` substring starting at `start_brace`."""
    depth = 0
    index = start_brace
    in_string: str | None = None
    while index < len(text):
        ch = text[index]
        if in_string is not None:
            if ch == "\\":
                index += 2
                continue
            if ch == in_string:
                in_string = None
        elif ch in ('"', "'"):
            in_string = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[start_brace : index + 1]
        index += 1
    return text[start_brace:]


def iter_function_blocks(stripped_text: str):
    """Yield `(start_offset, block_text)` for top-level C/C++ brace bodies."""
    depth = 0
    block_start = -1
    in_string: str | None = None
    index = 0
    while index < len(stripped_text):
        ch = stripped_text[index]
        if in_string is not None:
            if ch == "\\":
                index += 2
                continue
            if ch == in_string:
                in_string = None
        elif ch in ('"', "'"):
            in_string = ch
        elif ch == "{":
            if depth == 0:
                block_start = index
            depth += 1
        elif ch == "}":
            if depth > 0:
                depth -= 1
                if depth == 0 and block_start >= 0:
                    # Include up to 200 preceding characters so member-method
                    # scopes inside classes are also inspected per method.
                    yield block_start, stripped_text[block_start : index + 1]
                    block_start = -1
        index += 1


def iter_all_method_or_function_bodies(stripped_text: str):
    """Yield `(start_offset, body_text)` for function and C++ class method bodies."""
    brace_positions = []
    in_string: str | None = None
    index = 0
    while index < len(stripped_text):
        ch = stripped_text[index]
        if in_string is not None:
            if ch == "\\":
                index += 2
                continue
            if ch == in_string:
                in_string = None
        elif ch in ('"', "'"):
            in_string = ch
        elif ch == "{":
            # Heuristic: precede `{` with `)` (ignoring whitespace/qualifiers)
            prefix = stripped_text[max(0, index - 160) : index]
            if re.search(
                r"\)\s*(?:const\b|noexcept\b|override\b|final\b|\s)*$", prefix
            ) and not re.search(
                r"\b(?:if|for|while|switch|catch)\s*\([^()]*\)\s*$", prefix
            ):
                brace_positions.append(index)
        index += 1

    for pos in brace_positions:
        yield pos, iter_brace_blocks(stripped_text, pos)


def scan_app_file(
    path: Path, config: dict, repo_root: Path | None = None
) -> list[tuple[Path, int, str]]:
    try:
        raw_text = path.read_text()
    except UnicodeDecodeError:
        return []

    rel = normalize_relpath(path, repo_root)
    findings: list[tuple[Path, int, str]] = []

    is_unit_test = rel.startswith(config["app_unit_test_prefixes"])
    needs_upload = "gui_surface_upload" in raw_text
    needs_sndbuf = "SO_SNDBUF" in raw_text
    needs_wm = rel.endswith("mario-wm/main.c") or "wm_detach_client_stdio" in raw_text
    needs_pty = rel.endswith("terminal/main.c") or "terminal_drain_pty" in raw_text
    if not (needs_upload or needs_sndbuf or needs_wm or needs_pty):
        return findings

    stripped = strip_c_comments_preserving_lines(raw_text)

    # 1. Surface upload batching requires SO_SNDBUF expansion on client sockets.
    if needs_upload and "gui_surface_upload" in stripped and not is_unit_test:
        for match in BATCH_ENABLE_RE.finditer(stripped):
            if not SO_SNDBUF_RE.search(stripped):
                findings.append(
                    (
                        path,
                        line_number_at(stripped, match.start()),
                        "gui_surface_upload batching enabled without "
                        "setsockopt(..., SOL_SOCKET, SO_SNDBUF, ...)",
                    )
                )

    # 2. AF_UNIX SO_SNDBUF setsockopt fails with -ENOTCONN before connect()/socketpair().
    if needs_sndbuf:
        for body_offset, body in iter_all_method_or_function_bodies(stripped):
            sndbuf_match = SO_SNDBUF_RE.search(body)
            if not sndbuf_match:
                continue
            socket_match = SOCKET_CALL_RE.search(body)
            connect_match = CONNECT_CALL_RE.search(body)
            if socket_match and (
                connect_match is None or sndbuf_match.start() < connect_match.start()
            ):
                if socket_match.start() < sndbuf_match.start():
                    findings.append(
                        (
                            path,
                            line_number_at(
                                stripped, body_offset + sndbuf_match.start()
                            ),
                            "setsockopt(SO_SNDBUF) on AF_UNIX socket before "
                            "connect()/socketpair() (returns -ENOTCONN)",
                        )
                    )

    # 3. Compositor-launched ordinary client fork paths must detach stdio.
    if rel.endswith("mario-wm/main.c") or "wm_detach_client_stdio" in stripped:
        for match in WM_LAUNCH_FUNC_RE.finditer(stripped):
            brace_idx = stripped.find("{", match.end())
            if brace_idx < 0:
                continue
            body = iter_brace_blocks(stripped, brace_idx)
            if "fork(" in body and "wm_detach_client_stdio" not in body:
                findings.append(
                    (
                        path,
                        line_number_at(stripped, match.start()),
                        f"{match.group(1)}() forks client without "
                        "wm_detach_client_stdio()",
                    )
                )

    # 4. Terminal PTY drain must not mirror raw PTY bytes to STDOUT_FILENO.
    if rel.endswith("terminal/main.c") or "terminal_drain_pty" in stripped:
        drain_match = re.search(r"\bterminal_drain_pty\s*\([^;{]*\{", stripped)
        if drain_match:
            brace_idx = stripped.rfind("{", drain_match.start(), drain_match.end())
            body = iter_brace_blocks(stripped, brace_idx)
            echo_match = PTY_STDOUT_ECHO_RE.search(body)
            if echo_match:
                findings.append(
                    (
                        path,
                        line_number_at(stripped, brace_idx + echo_match.start()),
                        "terminal_drain_pty() writes PTY output to "
                        "STDOUT_FILENO (risks filling /tmp/mario-wm.log)",
                    )
                )

    return findings


def parse_gui_test_steps(lines: list[str]) -> list[tuple[int, str, str]]:
    """Parse a GUI `.test` file into `(line_number, directive, value)` steps."""
    steps: list[tuple[int, str, str]] = []
    heredoc_end: str | None = None
    heredoc_start = 0
    heredoc_key = ""
    heredoc_lines: list[str] = []

    for number, line in enumerate(lines, 1):
        if heredoc_end is not None:
            if line.strip() == heredoc_end:
                steps.append((heredoc_start, heredoc_key, "\n".join(heredoc_lines)))
                heredoc_end = None
                heredoc_lines = []
            else:
                heredoc_lines.append(line)
            continue

        stripped = line.strip()
        if not stripped or stripped.startswith("#") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        key = key.strip()
        value = value.strip()
        if key == "SERIAL" and value.startswith("<<"):
            heredoc_end = value[2:].strip()
            heredoc_start = number
            heredoc_key = key
            heredoc_lines = []
        else:
            steps.append((number, key, value))

    if heredoc_end is not None:
        steps.append((heredoc_start, heredoc_key, "\n".join(heredoc_lines)))
    return steps


def is_launcher_menu_dismiss_rect(value: str) -> bool:
    """Return True when GUI-CHECK-RECT only checks launcher popup dismissal."""
    parts = value.split()
    if len(parts) >= 2 and parts[0] == "10" and parts[1].isdigit():
        y = int(parts[1])
        return 590 <= y <= 735
    return False


def scan_gui_test_file(path: Path, config: dict) -> list[tuple[Path, int, str]]:
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        return []

    lines = text.splitlines()
    steps = parse_gui_test_steps(lines)
    findings: list[tuple[Path, int, str]] = []

    pending_launch: tuple[int, str] | None = None
    for number, key, value in steps:
        is_launcher_click = key == "GUI-CLICK" and any(
            asset in value for asset in config["gui_launcher_assets"]
        )
        is_bg_app_spawn = (
            key == "SERIAL"
            and "&" in value
            and "mario-wm" not in value
            and any(app in value for app in config["gui_test_apps"])
        )
        if is_launcher_click or is_bg_app_spawn:
            if SERIAL_READY_RE.search(value):
                pending_launch = None
            else:
                kind = (
                    "desktop launcher click"
                    if is_launcher_click
                    else "background GUI app launch"
                )
                pending_launch = (number, kind)
            continue

        if pending_launch is not None:
            if key in (
                "GUI-CHECK-SCREEN-RECT",
                "GUI-CHECK-SCREEN",
                "GUI-CLICK",
                "GUI-RIGHT-CLICK",
                "GUI-POINT",
                "SERIAL-WAIT-FILE",
            ):
                pending_launch = None
            elif key == "GUI-CHECK-RECT":
                if not is_launcher_menu_dismiss_rect(value):
                    pending_launch = None
            elif key == "SERIAL":
                if SERIAL_READY_RE.search(value):
                    pending_launch = None
            elif key in ("GUI-SEND-KEY", "GUI-SEND-KEYS", "GUI-TYPE"):
                if key == "GUI-SEND-KEY" and value in config["gui_test_vt_keys"]:
                    continue
                launch_line, launch_kind = pending_launch
                findings.append(
                    (
                        path,
                        number,
                        f"{key} after {launch_kind} (line {launch_line}) "
                        "without waiting for window/surface readiness "
                        "(pgrep alone races window focus)",
                    )
                )
                pending_launch = None

    return findings


def scan_test_mnt_isolation(path: Path) -> list[tuple[Path, int, str]]:
    """Flag mutations of shared `/mnt` when the test does not mount `/mnt`."""
    try:
        text = path.read_text()
    except UnicodeDecodeError:
        return []

    if "/mnt" not in text or MOUNT_MNT_RE.search(text):
        return []

    findings: list[tuple[Path, int, str]] = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if MNT_MUTATION_RE.search(line):
            findings.append(
                (
                    path,
                    number,
                    "mutates shared /mnt without mounting a dedicated fixture; "
                    "use a unique directory under /tmp instead",
                )
            )
    return findings


def iter_app_files(root: Path, config: dict, repo_root: Path | None) -> list[Path]:
    if root.is_file():
        return [root] if root.suffix in APP_SOURCE_SUFFIXES else []

    files: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        current = Path(dirpath)
        pruned = []
        for name in sorted(dirnames):
            if name in {"__pycache__", ".git", ".deps", ".libs", "CMakeFiles"}:
                continue
            candidate = current / name
            rel = normalize_relpath(candidate, repo_root)
            if any(pattern.search(rel) for pattern in config["app_exclude_dirs"]):
                continue
            pruned.append(name)
        dirnames[:] = pruned

        for filename in sorted(filenames):
            file_path = current / filename
            if file_path.suffix in APP_SOURCE_SUFFIXES:
                files.append(file_path)
    return files


def detect_domain(path: Path, repo_root: Path | None) -> str:
    rel = normalize_relpath(path, repo_root)
    parts = Path(rel).parts
    if path.suffix == ".test" or (parts and parts[0] == "tests"):
        return "tests"
    if parts and parts[0] == "src":
        return "src"
    if parts and parts[0] == "app":
        return "app"
    if path.is_file():
        if path.suffix in {".cc", ".cpp", ".cxx", ".hh", ".hpp"}:
            return "app"
        if path.suffix == ".c":
            text = path.read_text(errors="ignore")
            if (
                "gui_surface_upload" in text
                or "SO_SNDBUF" in text
                or "wm_detach_client_stdio" in text
            ):
                return "app"
            return "src"
    return "mixed"


def scan_target(
    target: Path, config: dict, repo_root: Path | None = None
) -> list[tuple[Path, int, str]]:
    domain = detect_domain(target, repo_root)
    findings: list[tuple[Path, int, str]] = []

    if domain == "src":
        c_files = [target] if target.is_file() else sorted(target.rglob("*.c"))
        for file_path in c_files:
            findings.extend(
                check_kernel_locks.scan_file(file_path, config["kernel_rules"])
            )
        return findings

    if domain == "app":
        for file_path in iter_app_files(target, config, repo_root):
            findings.extend(scan_app_file(file_path, config, repo_root))
        return findings

    if domain == "tests":
        test_files = [target] if target.is_file() else sorted(target.rglob("*.test"))
        for file_path in test_files:
            rel = normalize_relpath(file_path, repo_root)
            is_gui_test = (
                "/gui/" in f"/{rel}"
                or "GUI-VISUAL-TEST:" in file_path.read_text(errors="ignore")
            )
            if is_gui_test:
                findings.extend(scan_gui_test_file(file_path, config))
            findings.extend(scan_test_mnt_isolation(file_path))
        return findings

    # Mixed directory (e.g. repository root `.`): scan existing `src`, `app`, `tests`.
    for subdir in ("src", "app", "tests"):
        candidate = target / subdir
        if candidate.exists():
            findings.extend(scan_target(candidate, config, repo_root))
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check Mario kernel, app/GUI, and test invariants."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        default=["src", "app", "tests"],
        help="files or directories to scan (default: src app tests)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=DEFAULT_CONFIG,
        help=f"JSON rules file (default: {DEFAULT_CONFIG})",
    )
    args = parser.parse_args(argv)

    config = load_config(args.config)
    repo_root = Path.cwd()
    findings: list[tuple[Path, int, str]] = []

    for raw_path in args.paths:
        target = Path(raw_path)
        if not target.exists():
            print(f"{target}: path does not exist", file=sys.stderr)
            return 2
        findings.extend(scan_target(target, config, repo_root))

    for file_path, line, message in findings:
        print(f"{file_path}:{line}: {message}")

    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
