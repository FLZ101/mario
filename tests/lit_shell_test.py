#!/usr/bin/env python3
r"""A very small lit-style runner for Mario shell behavior tests.

Test files use a tiny subset of lit syntax:

    RUN: echo hello > /root/lit.txt
    RUN: cat /root/lit.txt
    CHECK: hello
    CHECK-NOT: goodbye
    CHECK-REGEX: hello\s+world
    RUN: wc -l <<'EOF'
    one
    two
    EOF
    EXIT:

Run:

    python3 tests/lit_shell_test.py tests/shell
"""

import argparse
import os
import re
import sys
import time

try:
    import pexpect
except ImportError:
    print("pexpect is required: pip install pexpect", file=sys.stderr)
    sys.exit(2)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.environ.get("QEMU", "qemu-system-i386")
KERNEL = os.path.join(ROOT, "src", "kernel", "kernel.exe")
INITRD = os.path.join(ROOT, "rd", "rdx")


def prompt_pattern(literal):
    ansi = r"\x1b(?:\[[0-9;?]*[A-Za-z]|[=>])"
    optional_ansi = r"(?:" + ansi + r")*"
    body = "".join(re.escape(ch) + optional_ansi for ch in literal)
    return r"(?m)^\r*" + optional_ansi + body


BOOT_PROMPT = prompt_pattern("/ # ")
PROMPT = prompt_pattern("MARIO_TEST_PROMPT> ")
DEFAULT_TIMEOUT = 30

HEREDOC = re.compile(r"<<-?\s*(?:'([^']+)'|\"([^\"]+)\"|([A-Za-z_][A-Za-z0-9_]*))")


def _heredoc_delimiter(command):
    match = HEREDOC.search(command)
    if not match:
        return None
    return next(group for group in match.groups() if group is not None)


class PexpectTimeout(TimeoutError):
    pass


def _pexpect_expect(child, pattern, timeout, description):
    try:
        return child.expect(pattern, timeout=timeout)
    except pexpect.exceptions.TIMEOUT:
        last = str(getattr(child, "before", ""))[-500:]
        raise PexpectTimeout(
            f"{description} timed out after {timeout:g}s\nLast output: {last!r}"
        ) from None


class LitTest:
    def __init__(self, path):
        self.path = path
        self.runs = []
        self.actions = []
        self.checks = []
        self.check_nots = []
        self.check_regexes = []

    def parse(self):
        heredoc_delimiter = None
        heredoc_command = None
        heredoc_lines = []

        with open(self.path, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")

                if heredoc_delimiter is not None:
                    if line.strip() == heredoc_delimiter:
                        command = "\n".join(
                            [heredoc_command] + heredoc_lines + [heredoc_delimiter]
                        )
                        self.runs.append(command)
                        self.actions.append(("run", command))
                        heredoc_delimiter = None
                        heredoc_command = None
                        heredoc_lines = []
                    else:
                        heredoc_lines.append(line)
                    continue

                body = line[1:] if line.startswith("#") else line
                if body.startswith("RUN:"):
                    command = body[len("RUN:") :]
                    if command.startswith(" "):
                        command = command[1:]
                    delimiter = _heredoc_delimiter(command)
                    if delimiter:
                        heredoc_delimiter = delimiter
                        heredoc_command = command
                        heredoc_lines = []
                    else:
                        self.runs.append(command)
                        self.actions.append(("run", command))
                elif body.startswith("SEND-RAW:"):
                    data = (
                        body[len("SEND-RAW:") :]
                        .strip()
                        .encode()
                        .decode("unicode_escape")
                    )
                    self.actions.append(("send-raw", data))
                elif body.startswith("SEND:"):
                    data = body[len("SEND:") :].strip()
                    self.actions.append(("send", data))
                elif body.startswith("EXPECT:"):
                    needle = body[len("EXPECT:") :].strip()
                    self.actions.append(("expect", needle))
                elif body.startswith("SLEEP:"):
                    seconds = float(body[len("SLEEP:") :].strip())
                    self.actions.append(("sleep", seconds))
                elif body.startswith("TIMEOUT:"):
                    seconds = float(body[len("TIMEOUT:") :].strip())
                    if seconds <= 0:
                        raise ValueError(f"{self.path}: TIMEOUT must be positive")
                    self.actions.append(("set-timeout", seconds))
                elif body.startswith("PUSH-PROMPT:"):
                    prompt = body[len("PUSH-PROMPT:") :].strip()
                    if not prompt:
                        raise ValueError(f"{self.path}: empty PUSH-PROMPT value")
                    self.actions.append(("push-prompt", prompt))
                elif body.startswith("POP-PROMPT:"):
                    self.actions.append(("pop-prompt", None))
                elif body.startswith("EXIT:"):
                    break
                elif body.startswith("CHECK-REGEX:"):
                    self.check_regexes.append(body[len("CHECK-REGEX:") :].strip())
                elif body.startswith("CHECK-NOT:"):
                    self.check_nots.append(body[len("CHECK-NOT:") :].strip())
                elif body.startswith("CHECK:"):
                    self.checks.append(body[len("CHECK:") :].strip())

        if heredoc_delimiter is not None:
            raise ValueError(f"{self.path}: unterminated heredoc {heredoc_delimiter!r}")

        return self


def require_files():
    missing = [p for p in (KERNEL, INITRD) if not os.path.exists(p)]
    if missing:
        print("Missing build outputs:", ", ".join(missing), file=sys.stderr)
        sys.exit(2)


def start_qemu(qemu):
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
        "-no-reboot",
        "-no-shutdown",
    ]
    child = pexpect.spawn(" ".join(cmd), encoding="latin-1", timeout=30)
    _pexpect_expect(child, BOOT_PROMPT, 30, "wait for boot prompt")
    child.sendline("PS1='MARIO_TEST_PROMPT> '")
    _pexpect_expect(child, PROMPT, 10, "wait for MARIO_TEST_PROMPT")
    return child


def run_lit_test(test, qemu):
    child = start_qemu(qemu)
    output = []
    prompts = [PROMPT]
    timeout = DEFAULT_TIMEOUT

    try:
        for action, value in test.actions:
            if action == "run":
                child.sendline(value)
                _pexpect_expect(child, prompts[-1], timeout, f"RUN {value!r}")
                output.append(child.before)
            elif action == "send":
                child.sendline(value)
                output.append(child.before)
            elif action == "send-raw":
                child.send(value)
                output.append(child.before)
            elif action == "expect":
                if value:
                    _pexpect_expect(
                        child, re.escape(value), timeout, f"EXPECT {value!r}"
                    )
                    output.append(child.before + child.after)
                else:
                    _pexpect_expect(child, prompts[-1], timeout, "EXPECT prompt")
                    output.append(child.before)
            elif action == "sleep":
                time.sleep(value)
            elif action == "set-timeout":
                timeout = value
            elif action == "push-prompt":
                prompts.append(prompt_pattern(value))
            elif action == "pop-prompt":
                if len(prompts) > 1:
                    prompts.pop()
    finally:
        child.sendline("halt")
        child.close(force=True)

    text = "\n".join(output)
    failures = []

    for needle in test.checks:
        if needle not in text:
            failures.append(f"CHECK: {needle!r}")

    for needle in test.check_nots:
        if needle in text:
            failures.append(f"CHECK-NOT: {needle!r}")

    for pattern in test.check_regexes:
        try:
            matched = re.search(pattern, text) is not None
        except re.error as exc:
            failures.append(f"CHECK-REGEX: {pattern!r} (invalid regex: {exc})")
            continue
        if not matched:
            failures.append(f"CHECK-REGEX: {pattern!r}")

    return failures, text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "paths",
        nargs="+",
        metavar="path",
        help="test files or directories containing .test files",
    )
    parser.add_argument("--qemu", default=QEMU)
    parser.add_argument("--test", help="run only this test file")
    parser.add_argument("--list", action="store_true", help="list discovered tests")
    args = parser.parse_args()

    require_files()
    test_paths = []
    for path in args.paths:
        if os.path.isdir(path):
            for root, _dirs, files in os.walk(path):
                for name in files:
                    if name.endswith(".test"):
                        test_paths.append(os.path.join(root, name))
        elif os.path.isfile(path):
            test_paths.append(path)
        else:
            parser.error(f"path does not exist: {path}")

    test_paths = sorted(set(os.path.abspath(p) for p in test_paths))
    if args.test:
        test_paths = [
            p for p in test_paths if os.path.basename(p) == os.path.basename(args.test)
        ]

    if args.list:
        for p in test_paths:
            print(os.path.relpath(p, ROOT))
        sys.exit(0)

    tests = [LitTest(p).parse() for p in test_paths]

    if not tests:
        print(f"No .test files found in: {' '.join(args.paths)}", file=sys.stderr)
        sys.exit(2)

    failed = False
    for test in tests:
        name = os.path.relpath(test.path, ROOT)
        try:
            failures, text = run_lit_test(test, args.qemu)
        except PexpectTimeout as exc:
            failed = True
            print(f"FAIL: {name}")
            print(f"  {exc}")
            continue
        if failures:
            failed = True
            print(f"FAIL: {name}")
            for failure in failures:
                print(f"  {failure}")
            print("  output:")
            for line in text.splitlines():
                print(f"    {line}")
        else:
            print(f"PASS: {name}")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
