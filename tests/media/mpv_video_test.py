"""Exercise native mpv video output through QEMU's screen and keyboard."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from mpv_decode_test import MPV, media_disk, run_checked
from lit_gui_test import (
    GuiTestError,
    Qmp,
    dump_screen,
    qemu_command,
    send_key,
    send_keys,
    send_mouse_button,
    send_mouse_position,
)
from lit_shell_test import BOOT_PROMPT, HD1_IMAGE, QEMU, ROOT
from pexpect_utils import make_pexpect_raw
import pexpect


CONTROLS = (
    "p cycle pause; print-text media-paused:${pause}\n"
    "s seek 5.5 absolute+exact; print-text media-seek\n"
)
SAMPLE = (216, 136, 232, 152)
MAX_CENTER = (504, 376, 520, 392)
MAX_TOP = (100, 50, 116, 66)
HD_SIZE = (1920, 1080)
HD_EDGES = ((128, 160, 144, 176), (1776, 160, 1792, 176))
HD_ASSET = "video-markers-1080p.mp4"
HD_FRAMES = 12
HD_BITS = tuple((632 + bit * 192, 456, 648 + bit * 192, 472) for bit in range(4))
HD_OPTIONS = (
    "--pause --framedrop=no "
    "--term-status-msg='media-drops:${frame-drop-count}/${decoder-frame-drop-count} "
    "media-eof:${eof-reached}'"
)
BLACK = (0, 0, 0)
RED = (255, 0, 0)
BLUE = (0, 0, 255)
WHITE = (255, 255, 255)


def vbe_boot_disk(work, kernel=None, *, size=(1024, 768)):
    boot = work / "hd1.img"
    subprocess.run(
        ["cp", "--reflink=auto", "--sparse=always", HD1_IMAGE, boot], check=True
    )
    table = json.loads(subprocess.check_output(["sfdisk", "--json", boot]))[
        "partitiontable"
    ]
    offset = table["partitions"][0]["start"] * table["sectorsize"]
    width, height = size
    menu = Path(ROOT) / f"qemu/grub4dos-0.4.6a/menu.lst.vbe_{width}x{height}"
    subprocess.run(
        ["mcopy", "-o", "-i", f"{boot}@@{offset}", menu, "::menu.lst"], check=True
    )
    if kernel is not None:
        kernel_offset = table["partitions"][-1]["start"] * table["sectorsize"]
        subprocess.run(
            ["mcopy", "-o", "-i", f"{boot}@@{kernel_offset}", kernel, "::kernel.exe"],
            check=True,
        )
    return boot


def sample_matches(screen, rectangle, color):
    return all(
        abs(low - wanted) <= 4 and abs(high - wanted) <= 4
        for (low, high), wanted in zip(screen.crop(rectangle).getextrema(), color)
    )


def expect_screen(qmp, reason, predicate, timeout=5, interval=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        screen = dump_screen(qmp)
        if predicate(screen):
            return screen
        time.sleep(interval)
    raise AssertionError(f"screen did not show {reason} within {timeout:g}s")


def expect_color(qmp, color, rectangle=SAMPLE, *, timeout=5):
    return expect_screen(
        qmp,
        f"{color} at {rectangle}",
        lambda screen: sample_matches(screen, rectangle, color),
        timeout=timeout,
    )


def maximize_player(qmp, screen):
    sample_x, sample_y, _, _ = SAMPLE

    def blue(column, row):
        return sample_matches(screen, (column, row, column + 1, row + 1), BLUE)

    right = next(
        column for column in range(sample_x, screen.width) if not blue(column, sample_y)
    )
    top = next(row + 1 for row in range(sample_y, -1, -1) if not blue(sample_x, row))
    send_mouse_position(qmp, right - 24, top - 12)
    send_mouse_button(qmp, "left", True)
    send_mouse_button(qmp, "left", False)


def start_player(child, asset, options=""):
    run_checked(
        child,
        "/mnt/mpv --no-config --no-msg-color --no-input-terminal --vo=mario --msg-level=vo/mario=trace "
        f"--input-conf=/mnt/controls.conf {options} /mnt/{asset} "
        ">/tmp/mpv.log 2>&1 & media_pid=$!",
    )


def finish_player(child, marker, *, size=(320, 180)):
    output = run_checked(
        child, 'wait "$media_pid"; result=$?; cat /tmp/mpv.log; (exit "$result")'
    )
    width, height = size
    if marker not in output or f"VO: [mario] {width}x{height} yuv420p" not in output:
        raise AssertionError(f"unexpected mpv output:\n{output}")
    return output


def expect_log(child, pattern):
    run_checked(
        child,
        f"timeout 5 sh -c 'until grep -q \"{pattern}\" /tmp/mpv.log; do sleep 0.1; done'",
    )


def prepare_guest(child, qmp):
    child.expect(BOOT_PROMPT, timeout=45)
    run_checked(child, "mount -t ext2 -o ro /dev/hdd /mnt")
    run_checked(child, "while ! test -e /tmp/mario-wm.started; do sleep 1; done")
    send_key(qmp, "f8")
    return expect_color(qmp, (31, 41, 51))


def verify_filemanager_open(child, qmp):
    run_checked(
        child,
        "mkdir /tmp/fm-media && cp /mnt/video-markers.mp4 '/tmp/fm-media/Video with spaces.MP4'",
    )
    run_checked(
        child,
        "HOME=/tmp /usr/bin/filemanager /tmp/fm-media >/tmp/fm-media.log 2>&1 & fm_pid=$!",
    )
    row = (410, 250, 416, 256)
    expect_color(qmp, WHITE, row)
    send_mouse_position(qmp, 315, 256)
    send_mouse_button(qmp, "left", True)
    send_mouse_button(qmp, "left", False)
    expect_color(qmp, (188, 217, 251), row)
    send_keys(qmp, ("ctrl", "ret"))
    expect_color(qmp, RED, (320, 216, 336, 232))
    expect_color(qmp, (188, 217, 251), row, timeout=12)
    send_keys(qmp, ("ctrl", "q"))
    run_checked(child, 'wait "$fm_pid"')


def verify_guest(child, qmp):
    desktop = prepare_guest(child, qmp).crop(SAMPLE).tobytes()

    start_player(child, "video-markers.mp4", "--pause")
    expect_color(qmp, BLACK)
    run_checked(child, "(: 3>/dev/pcm0)")
    send_key(qmp, "p")
    expect_color(qmp, RED)
    send_key(qmp, "p")
    expect_log(child, "media-paused:yes")
    for _ in range(5):
        if not sample_matches(dump_screen(qmp), SAMPLE, RED):
            raise AssertionError("paused video did not retain the red frame")
        time.sleep(0.1)
    send_key(qmp, "s")
    maximize_player(qmp, expect_color(qmp, BLUE))
    expect_screen(
        qmp,
        "maximized video with black letterbox bars",
        lambda screen: sample_matches(screen, MAX_CENTER, BLUE)
        and sample_matches(screen, MAX_TOP, BLACK),
    )
    send_keys(qmp, ("meta_l", "w"))
    output = finish_player(child, "(Quit)")
    if "AO:" in output:
        raise AssertionError(f"video-only file initialized audio:\n{output}")
    expect_screen(
        qmp,
        "desktop after close",
        lambda screen: screen.crop(SAMPLE).tobytes() == desktop,
    )

    start_player(child, "flash-beep.mp4", "--ao=mario --pause --start=1.07")
    expect_color(qmp, WHITE)
    expect_log(child, r"AO: \[mario\] 48000Hz stereo")
    output = run_checked(child, "(: 3>/dev/pcm0)", success=False)
    if "Resource busy" not in output:
        raise AssertionError(f"expected exclusive PCM ownership:\n{output}")
    send_keys(qmp, ("meta_l", "w"))
    output = finish_player(child, "(Quit)")
    if "AO: [mario] 48000Hz stereo" not in output:
        raise AssertionError(f"A/V playback did not open Mario PCM:\n{output}")
    run_checked(child, "(: 3>/dev/pcm0)")

    run_checked(
        child,
        "/usr/bin/mpv /mnt/flash-beep.mp4 >/tmp/mpv.log 2>&1 & media_pid=$!",
    )
    expect_color(qmp, WHITE)
    output = finish_player(child, "(End of file)")
    if "AO: [mario] 48000Hz stereo" not in output:
        raise AssertionError(f"packaged mpv did not initialize PCM:\n{output}")
    expect_screen(
        qmp,
        "desktop after EOF",
        lambda screen: screen.crop(SAMPLE).tobytes() == desktop,
    )
    run_checked(child, "(: 3>/dev/pcm0)")

    run_checked(
        child,
        "/usr/bin/mpv /mnt/video-markers.mp4 >/tmp/mpv.log 2>&1 & media_pid=$!",
    )
    sample = (256, 176, 272, 192)
    expect_color(qmp, RED, sample)
    send_key(qmp, "right")
    expect_color(qmp, BLUE, sample, timeout=2)
    finish_player(child, "(End of file)")

    output = run_checked(
        child, "/usr/bin/mpv /mnt/stereo-markers-48000.mp4", success=False
    )
    if "Expected an MP4 file with H.264 video" not in output:
        raise AssertionError(f"packaged mpv did not reject audio-only MP4:\n{output}")

    verify_filemanager_open(child, qmp)


def expect_no_drops(output):
    counts = re.findall(r"media-drops:(\d+/\d+)", output)
    if not counts or set(counts) != {"0/0"}:
        raise AssertionError(
            f"expected zero VO/decoder drops, got {sorted(set(counts))}"
        )


def verify_1080p(child, qmp):
    desktop = prepare_guest(child, qmp)
    if desktop.size != HD_SIZE:
        raise AssertionError(f"expected a 1920x1080 framebuffer, got {desktop.size}")

    def expect_frame(number):
        color = BLUE if number < HD_FRAMES // 2 else RED

        def matches(screen):
            return all(
                sample_matches(screen, edge, color) for edge in HD_EDGES
            ) and all(
                sample_matches(screen, cell, WHITE if number & (1 << bit) else BLACK)
                for bit, cell in enumerate(HD_BITS)
            )

        expect_screen(
            qmp,
            f"maximized 1080p frame {number}",
            matches,
            timeout=10,
            interval=0.002,
        )

    start_player(child, HD_ASSET, HD_OPTIONS)
    expect_color(qmp, BLUE, timeout=15)
    send_key(qmp, "p")
    output = finish_player(child, "(End of file)", size=HD_SIZE)
    expect_no_drops(output)
    submitted = set(
        re.findall(r"Submitted surface [^\r\n]+ for PTS (\d+\.\d+)", output)
    )
    if len(submitted) != HD_FRAMES:
        raise AssertionError(
            f"expected {HD_FRAMES} distinct windowed frames, submitted {len(submitted)}"
        )

    start_player(child, HD_ASSET, HD_OPTIONS + " --keep-open=yes")
    maximize_player(qmp, expect_color(qmp, BLUE))
    expect_frame(0)
    send_key(qmp, "p")
    for number in range(1, HD_FRAMES):
        expect_frame(number)
    expect_log(child, "media-eof:yes")
    send_keys(qmp, ("meta_l", "w"))
    expect_no_drops(finish_player(child, "(Quit)", size=HD_SIZE))
    expect_screen(
        qmp,
        "desktop after 1080p close",
        lambda screen: all(
            screen.crop(edge).tobytes() == desktop.crop(edge).tobytes()
            for edge in HD_EDGES
        ),
    )


def run_case(smp, size, verify, media):
    width, height = size
    test_name = f"mpv video {width}x{height} (SMP={smp})"
    logs = Path(tempfile.mkdtemp(prefix="mario-mpv-video-"))
    with tempfile.TemporaryDirectory(prefix="mario-video-boot-") as work:
        qmp = Qmp(str(logs / "qmp.sock"))
        command = qemu_command(QEMU, qmp.path, smp, absolute_mouse=True)
        boot = vbe_boot_disk(Path(work), size=size)
        command = [arg.replace(HD1_IMAGE, str(boot)) for arg in command]
        command += [
            "-cpu",
            "max",
            "-drive",
            f"id=media,file={media},format=raw,if=none",
            "-device",
            "ide-hd,drive=media,bus=gui-ahci.3",
            "-device",
            "ich9-intel-hda,id=hda,msi=off",
            "-device",
            "hda-output,bus=hda.0,audiodev=audio",
            "-audiodev",
            "none,id=audio",
        ]
        with (logs / "serial.log").open("w", encoding="latin-1") as serial:
            child = pexpect.spawn(command[0], command[1:], encoding="latin-1")
            child.logfile_read = serial
            try:
                make_pexpect_raw(child)
                qmp.connect()
                verify(child, qmp)
            except (AssertionError, GuiTestError, pexpect.ExceptionPexpect) as error:
                try:
                    dump_screen(qmp).save(logs / "failure.png")
                    run_checked(child, "cat /tmp/mpv.log 2>/dev/null || true")
                except (
                    AssertionError,
                    OSError,
                    GuiTestError,
                    pexpect.ExceptionPexpect,
                ):
                    pass
                print(f"FAIL: {test_name}: {error}; logs: {logs}")
                return False
            finally:
                child.close(force=True)
                qmp.close()
    print(f"PASS: {test_name}; logs: {logs}")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    if not MPV.is_file() or not Path(HD1_IMAGE).is_file():
        parser.error("build the applications and disk images first")

    with media_disk(
        files=[("controls.conf", CONTROLS)],
        assets=("flash-beep.mp4", "video-markers.mp4", HD_ASSET),
    ) as media:
        if not run_case(args.smp, (1024, 768), verify_guest, media):
            return 1
        if not run_case(args.smp, HD_SIZE, verify_1080p, media):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
