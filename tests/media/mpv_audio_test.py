"""Check mpv AAC playback, pause, seek, and EOF against a QEMU WAV capture."""

import argparse
from itertools import groupby
from pathlib import Path
import sys
import tempfile
import time
import wave

from hda_test import quit_qemu, wav_frames
from lit_shell_test import BOOT_PROMPT, require_files
from mpv_decode_test import (
    MPV,
    RATES,
    finish_command,
    media_command,
    run_checked,
    start_command,
)
from pexpect_utils import make_pexpect_raw
import pexpect


MPV_COMMAND = "/mnt/mpv --no-config --no-msg-color --no-video --ao=mario"
BLOCK_MS = 10
BLOCK_FRAMES = 48 * BLOCK_MS
CONTROLS = (
    "p cycle pause; print-text media-paused:${pause}\n"
    "s seek 1.6 absolute+exact; print-text media-seek\n"
)


def expect_playback(child, pattern):
    if child.expect([pattern, BOOT_PROMPT], timeout=5):
        raise AssertionError(f"mpv exited before {pattern!r}:\n{child.before}")


def check_unavailable(child, error, redirect=""):
    output = run_checked(
        child,
        f"{MPV_COMMAND} --no-input-terminal /mnt/stereo-markers-48000.mp4 {redirect}",
        success=False,
    )
    if f"Opening /dev/pcm0: {error}" not in output or "AO: [null]" in output:
        raise AssertionError(f"expected explicit PCM failure ({error}):\n{output}")


def play(child, rate, seek):
    start_command(
        child,
        f"{MPV_COMMAND} --input-conf=/mnt/controls.conf "
        "--term-status-msg='media-time:${=time-pos}' "
        f"/mnt/stereo-markers-{rate}.mp4",
    )
    expect_playback(child, r"AO: \[mario\] 48000Hz stereo .*s16")
    expect_playback(child, r"media-time:0\.[123][0-9]+")
    child.send("p")
    expect_playback(child, "media-paused:yes")
    time.sleep(0.35)
    if seek:
        child.send("s")
        expect_playback(child, "media-seek")
    child.send("p")
    expect_playback(child, "media-paused:no")
    if "(End of file)" not in finish_command(child):
        raise AssertionError("mpv did not reach EOF")


def channel_masks(frames):
    for offset in range(0, len(frames) - BLOCK_FRAMES + 1, BLOCK_FRAMES):
        block = frames[offset : offset + BLOCK_FRAMES]
        left = sum(abs(left) for left, _ in block) > 500 * len(block)
        right = sum(abs(right) for _, right in block) > 500 * len(block)
        yield left + 2 * right


def check_capture(wav, seek):
    segments = [
        (channels, len(list(blocks)) * BLOCK_MS)
        for channels, blocks in groupby(channel_masks(wav_frames(wav)))
    ]
    segments = [(channels, ms) for channels, ms in segments if ms >= 30]
    if segments and segments[0][0] == 0:
        segments.pop(0)
    if segments and segments[-1][0] == 0:
        segments.pop()

    masks = [channels for channels, _ in segments]
    bounds = (
        [(100, 420), (30, 220), (350, 450)]
        if seek
        else [(100, 420), (30, 220), (80, 410), (450, 550), (450, 550), (450, 550)]
    )
    if masks != ([1, 0, 3] if seek else [1, 0, 1, 2, 0, 3]) or any(
        not lower <= ms <= upper for (_, ms), (lower, upper) in zip(segments, bounds)
    ):
        raise AssertionError(
            f"unexpected marker sequence (channel mask, ms): {segments}"
        )
    if not seek and not 450 <= segments[0][1] + segments[2][1] <= 550:
        raise AssertionError(
            f"pause lost or repeated part of the left marker: {segments}"
        )


def run_case(smp, logs, rate=None, seek=False):
    logs.mkdir()
    monitor = logs / "monitor.sock"
    wav = logs / "capture.wav"
    with media_command(smp, [("controls.conf", CONTROLS)]) as command:
        command += ["-monitor", f"unix:{monitor},server=on,wait=off"]
        if rate is not None:
            command += [
                "-append",
                "hda=on",
                "-device",
                "ich9-intel-hda,id=hda,msi=off",
                "-device",
                "hda-output,bus=hda.0,audiodev=audio",
                "-audiodev",
                f"wav,id=audio,path={wav},timer-period=1000,"
                "out.frequency=48000,out.channels=2,out.format=s16",
            ]
        with (logs / "serial.log").open("w", encoding="latin-1") as serial:
            child = pexpect.spawn(
                command[0], command[1:], encoding="latin-1", timeout=45
            )
            child.logfile_read = serial
            try:
                make_pexpect_raw(child)
                child.expect(BOOT_PROMPT)
                run_checked(child, "mount -t ext2 -o ro /dev/hdd /mnt")
                if rate is None:
                    check_unavailable(child, "No such device")
                else:
                    play(child, rate, seek)
                    if rate == RATES[0] and not seek:
                        check_unavailable(child, "Resource busy", "3>/dev/pcm0")
            finally:
                try:
                    if child.isalive() and monitor.exists():
                        quit_qemu(child, monitor)
                finally:
                    child.close(force=True)

    if child.exitstatus != 0:
        raise AssertionError("QEMU did not exit cleanly")
    if rate is not None:
        check_capture(wav, seek)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    if not MPV.is_file():
        parser.error("mpv is missing; run make app first")
    require_files(require_kernel=True)

    logs = Path(tempfile.mkdtemp(prefix="mario-mpv-audio-"))
    try:
        run_case(args.smp, logs / "absent")
        for rate in RATES:
            run_case(args.smp, logs / f"{rate}-pause", rate)
            run_case(args.smp, logs / f"{rate}-seek", rate, seek=True)
    except (
        AssertionError,
        pexpect.ExceptionPexpect,
        OSError,
        wave.Error,
        EOFError,
    ) as error:
        print(f"FAIL: mpv AAC playback (SMP={args.smp}): {error}; logs: {logs}")
        return 1

    print(f"PASS: mpv AAC playback (SMP={args.smp}); logs: {logs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
