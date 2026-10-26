#!/usr/bin/env python3
r"""A very small lit-style runner for Mario shell behavior tests.

Test files use a tiny subset of lit syntax:

    RUN: echo hello > /root/lit.txt
    RUN: cat /root/lit.txt
    CHECK: hello
    CHECK-NOT: goodbye
    CHECK-REGEX: hello\s+world
    REQUIRES-SMP: >=4
    RUN: wc -l <<'EOF'
    one
    two
    EOF
    REQUIRES-HD:
    EXIT:

Run:

    python3 tests/lit_shell_test.py tests/shell
"""

import argparse
import os
import re
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

try:
    import pexpect
except ImportError:
    print("pexpect is required: pip install pexpect", file=sys.stderr)
    sys.exit(2)


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.environ.get("QEMU", "qemu-system-i386")
KERNEL = os.path.join(ROOT, "src", "kernel", "kernel.exe")
INITRD = os.path.join(ROOT, "rd", "rdx")
HD1_IMAGE = os.path.join(ROOT, "qemu", "hd1.img")
HD2_IMAGE = os.path.join(ROOT, "qemu", "hd2.img")
HD3_IMAGE = os.path.join(ROOT, "qemu", "hd3.img")


def prompt_pattern(literal):
    ansi = r"\x1b(?:\[[0-9;?]*[A-Za-z]|[=>])"
    optional_ansi = r"(?:" + ansi + r")*"
    body = "".join(re.escape(ch) + optional_ansi for ch in literal)
    return r"(?m)^\r*" + optional_ansi + body


BOOT_PROMPT = prompt_pattern("/ # ")
PROMPT = prompt_pattern("MARIO_TEST_PROMPT> ")
DEFAULT_TIMEOUT = 30


def parse_smp_requirement(spec):
    if not spec:
        return (">=", 2, ">=2")
    match = re.match(r"^\s*(>=|<=|!=|==|>|<|=)?\s*(\d+)\s*$", spec)
    if not match:
        raise ValueError(f"invalid REQUIRES-SMP value: {spec!r}")
    op = match.group(1) or "=="
    if op == "=":
        op = "=="
    return op, int(match.group(2)), spec.strip()


def smp_requirement_matches(requirement, smp):
    op, wanted, _spec = requirement
    if op == ">=":
        return smp >= wanted
    if op == "<=":
        return smp <= wanted
    if op == "!=":
        return smp != wanted
    if op == ">":
        return smp > wanted
    if op == "<":
        return smp < wanted
    return smp == wanted


HEREDOC = re.compile(r"<<-?\s*(?:'([^']+)'|\"([^\"]+)\"|([A-Za-z_][A-Za-z0-9_]*))")


def _heredoc_delimiter(command):
    match = HEREDOC.search(command)
    if not match:
        return None
    return next(group for group in match.groups() if group is not None)


class PexpectTimeout(TimeoutError):
    pass


class RealtimeOutput:
    def __init__(self, stream):
        self.stream = stream

    def write(self, data):
        self.stream.write(data)
        self.stream.flush()

    def flush(self):
        self.stream.flush()


def _pexpect_expect(child, pattern, timeout, description):
    try:
        return child.expect(pattern, timeout=timeout)
    except pexpect.exceptions.TIMEOUT:
        last = re.sub(r"\r+\n", "\n", str(getattr(child, "before", ""))[-500:])
        raise PexpectTimeout(
            f"{description} timed out after {timeout:g}s\nLast output: {last!r}"
        ) from None


class LitTest:
    def __init__(self, path):
        self.path = path
        self.requires_hd = False
        self.requires_smp = None
        self.runs = []
        self.actions = []

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
                elif body.startswith("REQUIRES-HD:"):
                    self.requires_hd = True
                elif body.startswith("REQUIRES-SMP:"):
                    spec = body[len("REQUIRES-SMP:") :].strip()
                    self.requires_smp = parse_smp_requirement(spec)
                elif body.startswith("EXIT:"):
                    break
                elif body.startswith("CHECK-REGEX:"):
                    self.actions.append(
                        ("check-regex", body[len("CHECK-REGEX:") :].strip())
                    )
                elif body.startswith("CHECK-NOT:"):
                    self.actions.append(
                        ("check-not", body[len("CHECK-NOT:") :].strip())
                    )
                elif body.startswith("CHECK:"):
                    self.actions.append(("check", body[len("CHECK:") :].strip()))

        if heredoc_delimiter is not None:
            raise ValueError(f"{self.path}: unterminated heredoc {heredoc_delimiter!r}")

        return self


def require_files():
    missing = [p for p in (KERNEL, INITRD) if not os.path.exists(p)]
    if missing:
        print("Missing build outputs:", ", ".join(missing), file=sys.stderr)
        sys.exit(2)


def start_qemu(qemu, realtime=False, with_disk=False, smp=1):
    cmd = [
        qemu,
        "-smp",
        str(smp),
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
    if with_disk:
        cmd += [
            "-snapshot",
            "-drive",
            f"id=mario-test-hd1,file={HD1_IMAGE},format=raw,if=none",
            "-drive",
            f"id=mario-test-hd2,file={HD2_IMAGE},format=raw,if=none",
            "-drive",
            f"id=mario-test-hd3,file={HD3_IMAGE},format=raw,if=none",
            "-device",
            "ahci,id=mario-test-ahci",
            "-device",
            "ide-hd,drive=mario-test-hd1,bus=mario-test-ahci.0",
            "-device",
            "ide-hd,drive=mario-test-hd2,bus=mario-test-ahci.1",
            "-device",
            "ide-hd,drive=mario-test-hd3,bus=mario-test-ahci.2",
        ]

    child = pexpect.spawn(" ".join(cmd), encoding="latin-1", timeout=30)
    if realtime:
        child.logfile_read = RealtimeOutput(sys.stdout)
    _pexpect_expect(child, BOOT_PROMPT, 30, "wait for boot prompt")
    child.sendline("PS1='MARIO_TEST_PROMPT> '")
    _pexpect_expect(child, PROMPT, 10, "wait for MARIO_TEST_PROMPT")
    return child


def run_lit_test(test, qemu, realtime=False, smp=1):
    # Start a fresh QEMU for this test so an SMP configuration from one case
    # cannot leak into the next case.
    child = start_qemu(qemu, realtime, test.requires_hd, smp)
    output = []
    prompts = [PROMPT]
    timeout = DEFAULT_TIMEOUT
    failures = []
    last_success_end = 0
    report_start = 0
    saw_failure = False

    def captured_text():
        chunks = [re.sub(r"\r+\n", "\n", chunk).rstrip("\r\n") for chunk in output]
        return "\n".join(chunks)

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
            elif action == "check":
                text = captured_text()
                if value in text:
                    last_success_end = len(text)
                else:
                    if not saw_failure:
                        report_start = last_success_end
                        saw_failure = True
                    failures.append(f"CHECK: {value!r}")
            elif action == "check-not":
                text = captured_text()
                if value not in text:
                    last_success_end = len(text)
                else:
                    if not saw_failure:
                        report_start = last_success_end
                        saw_failure = True
                    failures.append(f"CHECK-NOT: {value!r}")
            elif action == "check-regex":
                text = captured_text()
                try:
                    matched = re.search(value, text) is not None
                except re.error as exc:
                    if not saw_failure:
                        report_start = last_success_end
                        saw_failure = True
                    failures.append(f"CHECK-REGEX: {value!r} (invalid regex: {exc})")
                    continue
                if matched:
                    last_success_end = len(text)
                else:
                    if not saw_failure:
                        report_start = last_success_end
                        saw_failure = True
                    failures.append(f"CHECK-REGEX: {value!r}")
    finally:
        child.sendline("halt")
        child.close(force=True)

    return failures, captured_text(), report_start


def skipped_reason(test, missing_hd_images, smp):
    if test.requires_hd and missing_hd_images:
        return f"missing {', '.join(missing_hd_images)}"
    if test.requires_smp is not None and not smp_requirement_matches(
        test.requires_smp, smp
    ):
        _op, _wanted, spec = test.requires_smp
        return f"requires SMP {spec}"
    return None


def run_one(test, qemu, smp, realtime):
    name = os.path.relpath(test.path, ROOT)
    started = time.monotonic()
    try:
        failures, text, report_start = run_lit_test(
            test, qemu, realtime=realtime, smp=smp
        )
    except PexpectTimeout as exc:
        return name, started, None, exc, None, None
    except Exception as exc:
        return name, started, None, exc, None, None
    return name, started, failures, None, text, report_start


def report_one(name, started, failures, error, text, report_start, verbose):
    elapsed = time.monotonic() - started
    if error is not None:
        print(f"FAIL: {name} [{elapsed:.2f}s]", flush=True)
        print(error, flush=True)
        return True

    if failures:
        print(f"FAIL: {name} [{elapsed:.2f}s]", flush=True)
        for message in failures:
            print(message, flush=True)
        if not verbose and text is not None:
            context = text[report_start:].lstrip("\n")
            if context:
                print("\noutput since last success:\n")
                for line in context.splitlines():
                    print(line)
                print()
            else:
                print("(no output since last success)")
        return True

    if verbose:
        print(f"== {name} ==", flush=True)
        print()
    print(f"PASS: {name} [{elapsed:.2f}s]", flush=True)
    return False


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
    parser.add_argument(
        "--smp",
        type=int,
        default=1,
        help="number of QEMU CPUs to expose (default: 1)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="stream QEMU output while the test runs",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=1,
        help="run up to N tests concurrently (default: 1)",
    )
    args = parser.parse_args()
    if args.smp < 1:
        parser.error("--smp must be at least 1")
    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

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

    missing_hd_images = []
    if any(test.requires_hd for test in tests):
        missing_hd_images = [
            path
            for path in (HD1_IMAGE, HD2_IMAGE, HD3_IMAGE)
            if not os.path.exists(path)
        ]

    failed = False
    runnable = []
    for test in tests:
        name = os.path.relpath(test.path, ROOT)
        reason = skipped_reason(test, missing_hd_images, args.smp)
        if reason is not None:
            print(f"SKIP: {name} [{reason}]", flush=True)
            continue
        runnable.append(test)

    # Parallel QEMU output would interleave, so only stream in serial mode.
    realtime = args.verbose and args.jobs == 1

    def execute(test):
        return run_one(test, args.qemu, args.smp, realtime)

    if args.jobs == 1:
        results = map(execute, runnable)
        for result in results:
            failed = report_one(*result, args.verbose) or failed
    else:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(execute, test) for test in runnable]
            for future in as_completed(futures):
                failed = report_one(*future.result(), args.verbose) or failed

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
