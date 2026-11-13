"""Measure isolated GUI update latency on the common image without test sleeps."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import tempfile
import time

import pexpect
from PIL import Image, ImageChops

from lit_gui_test import (
    BOOT_MENU_DELAY,
    BOOT_PROMPT,
    DEFAULT_TIMEOUT,
    GuiTest,
    GuiTestError,
    PROMPT,
    QEMU,
    ROOT,
    Qmp,
    dump_screen,
    expect_prompt,
    qemu_command,
    run_serial_script,
    read_guest_frame_counter,
    send_key,
    send_keys,
    send_mouse_button,
    send_mouse_move,
    stop_init_window_manager,
    snapshot_guest_frame_counter,
    wait_for_frame_advance,
    wait_for_guest_file,
)
from lit_kernel import kernel_artifacts
from pexpect_utils import make_pexpect_raw, send_line


MARKERS = ((19, 83, 137), (137, 83, 19))
PAYLOAD_LINE = "0123456789abcdef" * 4
PAYLOAD_LINES = 512
PAYLOAD_BYTES = (len(PAYLOAD_LINE) + 1) * PAYLOAD_LINES
SAMPLE_SPACING = 0.15
SCREEN_POLL_INTERVAL = 0.01
WORK_CASES = {
    "idle",
    "cursor",
    "small-window-move",
    "visible-commit",
    "hidden-commit",
    "fullscreen-commit",
}


def summarize(samples: list[float], *, scale=1000, unit="ms") -> dict:
    if not samples:
        return {
            "count": 0,
            f"median_{unit}": None,
            f"p95_{unit}": None,
            f"max_{unit}": None,
        }
    ordered = sorted(samples)
    return {
        "count": len(samples),
        f"median_{unit}": statistics.median(samples) * scale,
        f"p95_{unit}": ordered[math.ceil(len(ordered) * 0.95) - 1] * scale,
        f"max_{unit}": max(samples) * scale,
    }


def parse_work_counters(output: str) -> dict:
    cases = {}
    fields = (
        "filled_pixels",
        "blitted_pixels",
        "flush_requests",
        "writer_calls",
        "written_bytes",
        "presentations",
    )
    for value in re.findall(r"gui-drawing-work: (\{[^\r\n]*\})", output):
        record = json.loads(value)
        name = record.get("case")
        if name not in WORK_CASES or name in cases:
            raise ValueError(f"unexpected or duplicate work case: {name!r}")
        if any(
            type(record.get(field)) is not int or record[field] < 0 for field in fields
        ):
            raise ValueError(f"invalid drawing counters for {name}")
        cases[name] = record
    if set(cases) != WORK_CASES:
        raise ValueError("missing drawing-work counters")
    return cases


def parse_payload_size(output: str) -> int:
    records = re.findall(r"(?:^|[\r\n])__GUI_BENCH_BYTES__([0-9]+)(?=[\r\n]|$)", output)
    if len(records) != 1 or int(records[0]) != PAYLOAD_BYTES:
        raise GuiTestError(f"incorrect terminal workload size: {records!r}")
    return int(records[0])


def marker_bounds(image: Image.Image, color: tuple[int, int, int]) -> tuple:
    difference = ImageChops.difference(image, Image.new("RGB", image.size, color))
    channels = difference.split()
    maximum = ImageChops.lighter(
        ImageChops.lighter(channels[0], channels[1]), channels[2]
    )
    bounds = maximum.point(lambda value: 255 if value == 0 else 0).getbbox()
    if bounds is None:
        raise GuiTestError(f"terminal marker {color} is not visible")
    return bounds


def wait_visible(qmp: Qmp, predicate, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate(dump_screen(qmp)):
            return
        time.sleep(SCREEN_POLL_INTERVAL)
    raise GuiTestError(f"visible update timed out after {timeout:.1f}s")


def run_samples(name, factory, qmp, samples, warmups, timeout, payload_bytes=0):
    result = {
        "case": name,
        "samples_seconds": [],
        "warmups_seconds": [],
        "failures": [],
        "payload_bytes": payload_bytes,
        "requested_samples": samples,
        "requested_warmups": warmups,
    }
    for index in range(warmups + samples):
        started = None
        try:
            action, predicate = factory(index)
            if predicate(dump_screen(qmp)):
                raise GuiTestError("sample target is already visible before injection")
            started = time.monotonic()
            action()
            wait_visible(qmp, predicate, timeout)
            elapsed = time.monotonic() - started
            target = "warmups_seconds" if index < warmups else "samples_seconds"
            result[target].append(elapsed)
        except Exception as error:
            result["failures"].append(
                {
                    "index": index,
                    "warmup": index < warmups,
                    "error": str(error),
                    "elapsed_seconds": (
                        None if started is None else time.monotonic() - started
                    ),
                }
            )
            break
        time.sleep(SAMPLE_SPACING)
    result["statistics"] = summarize(result["samples_seconds"])
    result["payload_bytes_per_second"] = (
        [payload_bytes / elapsed for elapsed in result["samples_seconds"]]
        if payload_bytes
        else []
    )
    if payload_bytes:
        result["throughput_statistics"] = summarize(
            result["payload_bytes_per_second"], scale=1, unit="bytes_per_second"
        )
    return result


@contextmanager
def guest(qemu, smp, serial_path):
    with kernel_artifacts(None, "disk-image-vbe_1024x768"), tempfile.TemporaryDirectory(
        prefix="mario-gui-perf-"
    ) as directory, serial_path.open("w", encoding="latin-1") as serial:
        command = qemu_command(qemu, str(Path(directory) / "qmp.sock"), smp)
        child = pexpect.spawn(
            command[0], command[1:], encoding="latin-1", timeout=DEFAULT_TIMEOUT
        )
        child.logfile_read = serial
        make_pexpect_raw(child)
        qmp = Qmp(str(Path(directory) / "qmp.sock"))
        try:
            time.sleep(BOOT_MENU_DELAY)
            child.expect(BOOT_PROMPT, timeout=30)
            send_line(child, f"PS1='{PROMPT}'")
            expect_prompt(child, 10, "set benchmark prompt")
            qmp.connect()
            stop_init_window_manager(child, DEFAULT_TIMEOUT)
            yield child, qmp, command
        except Exception:
            try:
                dump_screen(qmp).save(serial_path.with_suffix(".failure.png"))
            except Exception:
                pass
            try:
                send_line(
                    child, "cat /tmp/gui-bench.term; cat /tmp/mario-wm.log; dmesg"
                )
                expect_prompt(child, 10, "capture benchmark failure")
            except Exception:
                pass
            raise
        finally:
            try:
                if child.isalive():
                    send_line(child, "halt")
            finally:
                child.close(force=True)
                qmp.close()


def shell(child, command):
    send_line(child, command)
    return expect_prompt(child, DEFAULT_TIMEOUT, "benchmark setup")


def enter_fullscreen(child, qmp, color, timeout):
    baseline = snapshot_guest_frame_counter(child, timeout)
    send_keys(qmp, ("meta_l", "f"))
    wait_visible(qmp, lambda image: image.getpixel((16, 16)) == color, timeout)
    wait_for_frame_advance(
        lambda: read_guest_frame_counter(child, timeout), baseline, timeout
    )


def host_configuration():
    cpuinfo = Path("/proc/cpuinfo")
    memory = Path("/proc/meminfo")
    model = (
        re.search(r"^model name\s*:\s*(.+)$", cpuinfo.read_text(), re.MULTILINE)
        if cpuinfo.is_file()
        else None
    )
    total_memory = (
        re.search(r"^MemTotal:\s*(\d+) kB$", memory.read_text(), re.MULTILINE)
        if memory.is_file()
        else None
    )
    return {
        "cpu_model": model[1] if model else platform.processor(),
        "logical_cpus": os.cpu_count(),
        "cpu_affinity": (
            sorted(os.sched_getaffinity(0))
            if hasattr(os, "sched_getaffinity")
            else None
        ),
        "memory_bytes": int(total_memory[1]) * 1024 if total_memory else None,
        "load_average": list(os.getloadavg()) if hasattr(os, "getloadavg") else None,
    }


def metadata(args):
    sources = (
        "app/gui/desktop/mario-wm/main.c",
        "app/usr/terminal/main.c",
        "app/usr/terminal/vt.c",
        "app/gui/libmario-ui/src/flush.c",
        "app/gui/libmario-ui/src/surface.c",
        "app/test/gui-wm-busy/main.c",
        "tests/bench_gui_drawing.py",
        "src/kernel/kernel.exe",
        "app/gui/desktop/mario-wm/mario-wm.exe",
        "app/usr/terminal/terminal.exe",
        "app/test/gui-wm-busy/gui-wm-busy.exe",
        "rd/rdx",
        "util/mario-cc",
        "util/mario-musl-cc",
        ".build-env",
    )
    fingerprints = {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        for name in sources
        if (ROOT / name).is_file()
    }
    revision = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    dirty = subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=ROOT, text=True
    ).splitlines()
    deferred = []
    for name in ("gui-stress.test", "terminal-tmux-apps.test", "wm-many-windows.test"):
        fixture = GuiTest(ROOT / "tests/gui" / name).parse()
        if fixture.skip_reason is not None:
            deferred.append({"test": name, "reason": fixture.skip_reason})
    return {
        "timestamp": datetime.now().astimezone().isoformat(),
        "revision": revision,
        "dirty_files": dirty,
        "sha256": fingerprints,
        "host": platform.platform(),
        "machine": platform.machine(),
        "host_configuration": host_configuration(),
        "optimization_declared": args.optimization,
        "smp": args.smp,
        "samples": args.samples,
        "warmups": args.warmups,
        "sample_spacing_seconds": SAMPLE_SPACING,
        "timeout_seconds": args.timeout,
        "screen_poll_interval_seconds": SCREEN_POLL_INTERVAL,
        "deferred_tests": deferred,
        "measurement": "QMP injection through first visible marker; observational upper bound",
        "instrumentation": "QMP screendumps; separate test-only fill/blit and flush callbacks",
        "throughput": "fixed PTY burst bytes / visible final-marker latency, not sustained FPS",
        "workloads": {
            "cursor": {"delta_pixels": 32, "events": [1, 16]},
            "window_drag": {
                "content_size": [320, 192],
                "delta_pixels": 32,
                "events": [1, 16],
            },
            "terminal_burst": {
                "line": PAYLOAD_LINE,
                "lines": PAYLOAD_LINES,
                "payload_bytes": PAYLOAD_BYTES,
                "completion": "alternating RGB repaint",
            },
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, default=16)
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument(
        "--optimization", default="-O0", help="declare the image's build flag"
    )
    parser.add_argument("--qemu", default=QEMU)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if (
        args.smp < 1
        or args.samples < 1
        or args.warmups < 0
        or not math.isfinite(args.timeout)
        or args.timeout <= 0
    ):
        parser.error("invalid SMP, sample, warm-up, or timeout value")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    serial_path = args.output.with_suffix(".serial.log")
    failure = serial_path.with_suffix(".failure.png")
    failure.unlink(missing_ok=True)
    report = {
        "metadata": {},
        "results": [],
        "errors": [],
        "serial_log": str(serial_path),
    }
    try:
        with guest(args.qemu, args.smp, serial_path) as (child, qmp, command):
            report["metadata"] = metadata(args)
            report["metadata"].update(
                {
                    "qemu_command": command,
                    "qemu_version": qmp.command("query-version"),
                    "kvm": qmp.command("query-kvm"),
                }
            )
            report["metadata"]["accelerator"] = (
                "kvm" if report["metadata"]["kvm"]["enabled"] else "tcg"
            )
            counters = shell(child, "/usr/bin/test/gui-wm-busy.exe")
            report["work_counters"] = parse_work_counters(counters)
            script = f"""rm -f /tmp/mario-wm.*
/usr/bin/mario-wm >/tmp/mario-wm.log 2>&1 &
python -c 'open("/tmp/gui-bench.text", "wb").write((b"0123456789abcdef" * 4 + b"\\n") * {PAYLOAD_LINES})'
printf '\\n__GUI_BENCH_BYTES__%s\\n' "$(wc -c </tmp/gui-bench.text)"
cat >/tmp/gui-bench.run <<'BENCH_SCRIPT'
paint_marker() {{
    printf '\\033[?25l\\033[48;2;%sm\\033[2J' "$1"
    set -- $(stty size)
    columns=$(($2 - 1))
    for row in $(seq 1 "$1"); do
        printf '\\033[%s;1H%*s' "$row" "$columns" ''
    done
    printf '\\033[H'
}}
paint_marker '19;83;137'
phase=0
while IFS= read -r request; do
    cat /tmp/gui-bench.text
    if test "$phase" -eq 0; then
        paint_marker '137;83;19'
        phase=1
    else
        paint_marker '19;83;137'
        phase=0
    fi
done
BENCH_SCRIPT
"""
            setup = run_serial_script(
                child, script, DEFAULT_TIMEOUT, "prepare benchmark workload"
            )
            report["metadata"]["verified_payload_bytes"] = parse_payload_size(setup)
            wait_for_guest_file(child, "/tmp/mario-wm.started", DEFAULT_TIMEOUT)
            send_key(qmp, "f8")
            shell(
                child,
                '/usr/bin/terminal --command ". /tmp/gui-bench.run" '
                ">/tmp/gui-bench.term 2>&1 &",
            )
            wait_visible(
                qmp,
                lambda image: image.getpixel((144, 112)) == MARKERS[0],
                args.timeout,
            )
            viewport = list(marker_bounds(dump_screen(qmp), MARKERS[0]))
            report["metadata"]["resolution"] = list(dump_screen(qmp).size)
            sprite = (ROOT / "app/gui/desktop/assets/cursor-arrow-16.rgba").read_bytes()
            offset = next(
                index for index in range(0, len(sprite), 4) if sprite[index + 3]
            )
            sprite_offset = ((offset // 4) % 16, (offset // 4) // 16)
            sprite_color = tuple(sprite[offset : offset + 3])
            pointer = [16, 16]

            def position_pointer(x, y):
                send_mouse_move(qmp, x - pointer[0], y - pointer[1])
                pointer[:] = [x, y]
                point = (x + sprite_offset[0], y + sprite_offset[1])
                wait_visible(
                    qmp,
                    lambda image: image.getpixel(point) == sprite_color,
                    args.timeout,
                )

            def record(name, factory, payload=0):
                result = run_samples(
                    name,
                    factory,
                    qmp,
                    args.samples,
                    args.warmups,
                    args.timeout,
                    payload,
                )
                report["results"].append(result)
                print(f"{name}: {json.dumps(result['statistics'])}", flush=True)
                if result["failures"]:
                    raise GuiTestError(f"{name}: {result['failures']}")

            position_pointer(512, 600)
            for events in (1, 16):

                def cursor_factory(index, events=events):
                    delta = 32 if index % 2 == 0 else -32
                    pointer[0] += delta
                    point = (
                        pointer[0] + sprite_offset[0],
                        pointer[1] + sprite_offset[1],
                    )

                    def action():
                        for event in range(events):
                            send_mouse_move(qmp, delta // events, 0)

                    return action, lambda image: image.getpixel(point) == sprite_color

                record(f"cursor-{events}-events", cursor_factory)

            position_pointer(viewport[0] + 16, viewport[1] - 28)
            send_mouse_button(qmp, "left", True)
            time.sleep(SAMPLE_SPACING)
            for events in (1, 16):

                def drag_factory(index, events=events):
                    delta = 32 if index % 2 == 0 else -32
                    viewport[0] += delta
                    viewport[2] += delta
                    pointer[0] += delta
                    point = (
                        viewport[2] - 2 if delta > 0 else viewport[0] + 1,
                        viewport[1] + 8,
                    )

                    def action():
                        for event in range(events):
                            send_mouse_move(qmp, delta // events, 0)

                    return action, lambda image: image.getpixel(point) == MARKERS[0]

                record(f"window-drag-{events}-events", drag_factory)
            send_mouse_button(qmp, "left", False)
            phase = [0]

            def output_factory(index):
                phase[0] = 1 - phase[0]
                expected = MARKERS[phase[0]]
                point = (viewport[2] - 2, viewport[3] - 2)
                return (
                    lambda: send_key(qmp, "ret"),
                    lambda image: image.getpixel(point) == expected,
                )

            record("terminal-normal-burst", output_factory, PAYLOAD_BYTES)
            enter_fullscreen(child, qmp, MARKERS[phase[0]], args.timeout)
            phase[0] = 1 - phase[0]
            send_key(qmp, "ret")
            expected = MARKERS[phase[0]]
            wait_visible(
                qmp, lambda image: image.getpixel((990, 740)) == expected, args.timeout
            )
            viewport[:] = marker_bounds(dump_screen(qmp), expected)
            record("terminal-fullscreen-burst", output_factory, PAYLOAD_BYTES)
    except Exception as error:
        report["errors"].append(str(error))
    report["metadata"]["host_configuration_end"] = host_configuration()
    if failure.is_file():
        report["failure_screenshot"] = str(failure)
    temporary = args.output.with_name(args.output.name + ".new")
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(args.output)
    if report["errors"]:
        print("\n".join(report["errors"]), flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
