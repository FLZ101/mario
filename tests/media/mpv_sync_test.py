"""Measure mpv A/V sync at the QEMU framebuffer and WAV backend."""

import argparse
from collections import deque
import csv
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time
import wave

import pexpect
from PIL import Image

from mpv_decode_test import MPV, media_disk, run_checked, start_command
from mpv_video_test import (
    BLACK,
    SAMPLE,
    WHITE,
    expect_color,
    hda_boot_disk,
    sample_matches,
)
from lit_gui_test import GuiTestError, Qmp, qemu_command, send_key
from lit_shell_test import BOOT_PROMPT, HD1_IMAGE, QEMU
from pexpect_utils import make_pexpect_raw


CONTROLS = (
    "p cycle pause; print-text media-paused:${pause}\n"
    "s seek 1.5 absolute+exact; print-text media-seek\n"
)
DRIFT_SAMPLE = (176, 176, 192, 192)
RATE = 48000
FRAME_BYTES = 4
SCREEN_WIDTH = 1024
SCREEN_PIXEL_BYTES = 4
SAMPLE_ROWS = 16
POLL_S = 0.002
MAX_SKEW_MS = 100
DRIFT_SECONDS = 32
DRIFT_FRAMES = DRIFT_SECONDS * 15
WAV_HEADER_BYTES = 44


def vram_base(qmp):
    for bus in qmp.command("query-pci"):
        for device in bus["devices"]:
            if device["class_info"]["class"] == 0x0300:
                for region in device["regions"]:
                    if region["bar"] == 0 and region["address"] > 0:
                        return region["address"]
    raise AssertionError("std VGA framebuffer BAR was not assigned")


def read_scanlines(qmp, base, first_row, path):
    pitch = SCREEN_WIDTH * SCREEN_PIXEL_BYTES
    qmp.command(
        "pmemsave",
        {
            "val": base + first_row * pitch,
            "size": SAMPLE_ROWS * pitch,
            "filename": str(path),
        },
    )
    return Image.frombytes(
        "RGB", (SCREEN_WIDTH, SAMPLE_ROWS), path.read_bytes(), "raw", "BGRX"
    )


def frame_number(scanlines):
    guard = scanlines.crop((228, 0, 236, SAMPLE_ROWS)).getextrema()
    if not all(low >= 192 for low, _ in guard):
        return None
    number = 0
    for bit in range(9):
        left = 128 + 112 + 4 + bit * 16
        cell = scanlines.crop((left, 0, left + 8, SAMPLE_ROWS)).getextrema()
        if all(low >= 192 for low, _ in cell):
            number |= 1 << bit
        elif any(high > 63 for _, high in cell):
            return None
    return number if number < DRIFT_FRAMES else None


def scan_screen(child, qmp, logs, controls, drift, base):
    left, first_row, right, _ = DRIFT_SAMPLE if drift else SAMPLE
    rectangle = (left, 0, right, SAMPLE_ROWS)
    scanlines = logs / "scanlines.bin"
    last = None
    flashes = []
    observed = set()
    resumed = False
    sought = False
    deadline = time.monotonic() + (50 if drift else 15)
    with (logs / "screen.csv").open("w") as stream:
        trace = csv.writer(stream)
        trace.writerow(("request_s", "response_s", "marker", "frame"))
        while time.monotonic() < deadline:
            old = last
            before = time.monotonic()
            screen = read_scanlines(qmp, base, first_row, scanlines)
            after = time.monotonic()
            if drift:
                marker = screen.crop(rectangle).getextrema()
                frame = frame_number(screen)
                white = all(low >= 192 for low, _ in marker)
                black = all(high <= 63 for _, high in marker)
                if frame is None or not (white or black) or white != (
                    frame >= 15 and frame % 15 < 3
                ):
                    frame = None
                    white = black = False
            else:
                frame = None
                white = sample_matches(screen, rectangle, WHITE)
                black = sample_matches(screen, rectangle, BLACK)
            if frame is not None:
                observed.add(frame)
            color = "white" if white else "black" if black else "other"
            trace.writerow((before, after, color, frame))
            if white:
                if old is not None:
                    flashes.append((old, after))
                last = None
            elif black:
                last = before

            if controls and len(flashes) == 1 and black and not resumed:
                send_key(qmp, "p")
                time.sleep(0.45)
                send_key(qmp, "p")
                resumed = True
            if controls and len(flashes) == 2 and black and not sought:
                send_key(qmp, "s")
                sought = True

            done = child.expect([r"media-done:(\d+)\r*\n", pexpect.TIMEOUT], timeout=0.001)
            if done == 0:
                if child.match[1] != "0":
                    raise AssertionError(child.after.strip())
                return flashes, len(observed)
            time.sleep(POLL_S)
    raise AssertionError("mpv did not finish")


def captured_beeps(logs, timer_us):
    with wave.open(str(logs / "capture.wav")) as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (RATE, 2, 2):
            raise AssertionError(f"unexpected WAV format: {wav.getparams()}")
        samples = wav.readframes(wav.getnframes())
    offsets = deque()
    quiet = 0
    block_bytes = RATE * FRAME_BYTES // 200
    for offset in range(0, len(samples), block_bytes):
        block = samples[offset : offset + block_bytes]
        if any(
            abs(int.from_bytes(block[index : index + 2], "little", signed=True)) > 1500
            for index in range(0, len(block), FRAME_BYTES)
        ):
            if quiet >= 16:
                offsets.append(offset)
            quiet = 0
        else:
            quiet += 1

    beeps = []
    covered = 0
    max_chunk_ms = 0.0
    for line in (logs / "audio-timing.txt").read_text().splitlines():
        timestamp, wav_offset, size = line.split()
        offset = int(wav_offset) - WAV_HEADER_BYTES
        if offset < 0:
            continue
        size = int(size)
        if offset != covered or size % FRAME_BYTES:
            raise AssertionError("incomplete or unaligned WAV timing trace")
        covered += size
        chunk_ms = size / (RATE * FRAME_BYTES) * 1000
        max_chunk_ms = max(max_chunk_ms, chunk_ms)
        while offsets and offsets[0] < covered:
            offsets.popleft()
            uncertainty = chunk_ms / 1000 + 0.005 + timer_us / 1e6
            beeps.append((float(timestamp) - uncertainty, float(timestamp) + uncertainty))
    if covered != len(samples) or offsets or not beeps:
        raise AssertionError("QEMU WAV writes were not fully timestamped")
    return beeps, max_chunk_ms


def run_case(args, logs, probe, name, boot, media):
    logs.mkdir()
    qmp = Qmp(str(logs / "qmp.sock"))
    command = [
        arg.replace(HD1_IMAGE, str(boot)) for arg in qemu_command(QEMU, qmp.path, args.smp)
    ]
    command += [
        "-accel", args.accel,
        "-drive", f"id=media,file={media},format=raw,if=none",
        "-device", "ide-hd,drive=media,bus=gui-ahci.3",
        "-device", "ich9-intel-hda,id=hda,msi=off",
        "-device", "hda-output,bus=hda.0,audiodev=audio",
        "-audiodev", f"wav,id=audio,path={logs / 'capture.wav'},timer-period={args.timer_us},"
        "out.frequency=48000,out.channels=2,out.format=s16",
    ]
    env = os.environ | {
        "LD_PRELOAD": str(probe),
        "MARIO_WAV_CAPTURE": str(logs / "capture.wav"),
        "MARIO_WAV_TIMING": str(logs / "audio-timing.txt"),
    }
    drift = name == "drift"
    controls = name == "controls"
    asset = "flash-beep-drift.mp4" if drift else "flash-beep.mp4"
    with (logs / "serial.log").open("w", encoding="latin-1") as serial:
        child = pexpect.spawn(command[0], command[1:], env=env, encoding="latin-1", timeout=45)
        child.logfile_read = serial
        try:
            make_pexpect_raw(child)
            qmp.connect()
            child.expect(BOOT_PROMPT)
            run_checked(child, "mount -t ext2 -o ro /dev/hdd /mnt")
            run_checked(child, "while ! test -e /tmp/mario-wm.started; do sleep 1; done")
            send_key(qmp, "f8")
            expect_color(qmp, (31, 41, 51))
            base = vram_base(qmp)
            if not sample_matches(read_scanlines(qmp, base, SAMPLE[1], logs / "scanlines.bin"),
                                  (SAMPLE[0], 0, SAMPLE[2], SAMPLE_ROWS), (31, 41, 51)):
                raise AssertionError("VBE scanlines do not match the QMP desktop screenshot")
            run_checked(child, "free -k >/tmp/memory.log")
            start_command(
                child,
                "/mnt/mpv --no-config --no-msg-color --no-input-terminal --vo=mario --ao=mario "
                "--video-sync=audio --msg-level=vo/mario=trace,ao/mario=v "
                "--input-conf=/mnt/controls.conf --term-status-msg='vo-drops:${frame-drop-count}' "
                f"/mnt/{asset} >/tmp/mpv.log 2>&1 & media_pid=$!; "
                '(while kill -0 "$media_pid" 2>/dev/null; do free -k; sleep 0.5; done) '
                '>>/tmp/memory.log & wait "$media_pid"; printf "media-done:%s\\n" "$?"',
            )
            flashes, delivered = scan_screen(child, qmp, logs, controls, drift, base)
            child.expect(BOOT_PROMPT)
            output = run_checked(child, "cat /tmp/mpv.log")
            (logs / "mpv.log").write_text(output)
            memory = run_checked(child, "cat /tmp/memory.log")
            for marker in ("AO: [mario] 48000Hz", "VO: [mario] 320x180 yuv420p", "(End of file)"):
                if marker not in output:
                    raise AssertionError(f"missing {marker!r} in mpv.log")
            if controls and not all(marker in output for marker in (
                "media-paused:yes", "media-paused:no", "media-seek",
            )):
                raise AssertionError("mpv did not acknowledge pause, resume and seek")
            qmp.command("quit")
            child.expect(pexpect.EOF)
        finally:
            child.close(force=True)
            qmp.close()

    return report_case(logs, args.timer_us, name, flashes, delivered, output, memory)


def report_case(logs, timer_us, name, flashes, delivered, output, memory):
    beeps, chunk_ms = captured_beeps(logs, timer_us)
    drift = name == "drift"
    expected = DRIFT_SECONDS - 1 if drift else 4 if name == "controls" else 3
    matched = len(flashes) == len(beeps) == expected
    intervals = []
    if matched:
        intervals = [
            [(video[0] - audio[1]) * 1000, (video[1] - audio[0]) * 1000]
            for video, audio in zip(flashes, beeps)
        ]
    durations = [
        (float(total), float(convert))
        for total, convert in re.findall(
            r"Submitted surface \d+ in ([\d.]+) ms \(conversion ([\d.]+) ms\)", output
        )
    ]
    used = list(map(int, re.findall(r"^Mem:\s+\d+\s+(\d+)", memory, re.M)))
    if not used or not durations:
        raise AssertionError("missing guest memory or surface submission samples")
    playback, _, drain = output.partition("(End of file)")
    underruns = re.compile(r"PCM underruns: \+(\d+)")
    stats = {
        "skew_ms_video_minus_audio": intervals,
        "worst_case_skew_ms": max(
            (abs(bound) for interval in intervals for bound in interval), default=None
        ),
        "flash_count": len(flashes),
        "beep_count": len(beeps),
        "max_wav_chunk_ms": chunk_ms,
        "submitted": len(durations),
        "median_submission_ms": statistics.median(total for total, _ in durations),
        "median_conversion_ms": statistics.median(convert for _, convert in durations),
        "median_upload_ms": statistics.median(total - convert for total, convert in durations),
        "vo_drops": max(map(int, re.findall(r"vo-drops:(\d+)", output)), default=None),
        "visible_distinct_frames": delivered if drift else None,
        "pcm_underruns_during_playback": sum(map(int, underruns.findall(playback))),
        "pcm_underruns_after_eof": sum(map(int, underruns.findall(drain))),
        "guest_used_kib_before": used[0],
        "guest_peak_sampled_used_kib": max(used),
    }
    if drift and matched:
        midpoints = [statistics.mean(interval) for interval in intervals]
        stats["audio_pulse_span_ms"] = (
            statistics.mean(beeps[-1]) - statistics.mean(beeps[0])
        ) * 1000
        stats["drift_ms_first_to_last_5_pulses"] = (
            statistics.mean(midpoints[-5:]) - statistics.mean(midpoints[:5])
        )
    (logs / "stats.json").write_text(json.dumps(stats, indent=2) + "\n")
    if matched:
        skew = f"skew bounded by {stats['worst_case_skew_ms']:.1f} ms"
    else:
        skew = f"wanted {expected} pulses, got {len(flashes)} flashes and {len(beeps)} beeps"
    visible = f", {delivered}/{DRIFT_FRAMES} visible" if drift else ""
    print(
        f"{name}: {skew}; {len(durations)} submitted, {stats['vo_drops']} dropped, "
        f"{stats['pcm_underruns_during_playback']} playback underruns{visible}",
        flush=True,
    )
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=2)
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    parser.add_argument("--timer-us", type=int, default=5000)
    parser.add_argument("--kernel", type=Path, help="boot a different kernel on the disposable disk")
    parser.add_argument("--case", choices=("plain", "controls", "drift"), action="append")
    args = parser.parse_args()
    if args.timer_us <= 0:
        parser.error("--timer-us must be positive")
    if args.kernel is not None and not args.kernel.is_file():
        parser.error("--kernel must identify a built kernel executable")
    if not MPV.is_file() or not Path(HD1_IMAGE).is_file():
        parser.error("build the applications and disk images first")
    logs = Path(tempfile.mkdtemp(prefix="mario-mpv-sync-"))
    probe = logs / "wav-timing.so"
    print(
        f"mpv sync: {args.accel}, {args.smp} vCPUs, 256 MiB, WAV timer {args.timer_us} µs; logs: {logs}",
        flush=True,
    )
    try:
        subprocess.run([
            "cc", "-Wall", "-Wextra", "-O2", "-shared", "-fPIC",
            Path(__file__).with_name("qemu_wav_timing.c"), "-o", probe, "-ldl",
        ], check=True)
        (logs / "qemu-version.txt").write_bytes(subprocess.check_output([QEMU, "--version"]))
        (logs / "settings.json").write_text(json.dumps(vars(args), default=str, indent=2) + "\n")
        with tempfile.TemporaryDirectory(prefix="mario-sync-boot-") as work, media_disk(
            files=[("controls.conf", CONTROLS)], assets=("flash-beep.mp4", "flash-beep-drift.mp4")
        ) as media:
            boot = hda_boot_disk(Path(work), args.kernel)
            results = {
                name: run_case(args, logs / name, probe, name, boot, media)
                for name in dict.fromkeys(args.case or ("plain", "controls", "drift"))
            }
        failures = []
        if any(
            stats["worst_case_skew_ms"] is None or stats["worst_case_skew_ms"] > MAX_SKEW_MS
            for stats in results.values()
        ):
            failures.append(f"could not verify the {MAX_SKEW_MS} ms skew limit")
        if drift := results.get("drift"):
            span = drift.get("audio_pulse_span_ms", 0)
            if (
                drift["visible_distinct_frames"] < DRIFT_FRAMES - 5
                or abs(span - (DRIFT_SECONDS - 2) * 1000) > 300
            ):
                failures.append("drift run did not sustain 15 fps")
            if drift["pcm_underruns_during_playback"]:
                failures.append("drift run had PCM underruns")
        if failures:
            raise AssertionError("; ".join(failures))
    except (
        AssertionError,
        GuiTestError,
        OSError,
        pexpect.ExceptionPexpect,
        subprocess.CalledProcessError,
        wave.Error,
    ) as error:
        print(f"FAIL: mpv sync: {error}; logs: {logs}")
        return 1
    print(f"PASS: mpv sync; logs: {logs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
