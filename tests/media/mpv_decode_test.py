"""Decode the checked-in AAC and H.264 fixtures in a guest using a disposable disk."""

import argparse
from contextlib import contextmanager
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lit_shell_test import BOOT_PROMPT, QEMU, ROOT, qemu_command, require_files
from pexpect_utils import make_pexpect_raw, send_line
import pexpect


MEDIA = Path(ROOT) / "app/usr/video"
MPV = MEDIA / "install/bin/mpv"
ASSETS = Path(__file__).resolve().with_name("assets")
RATES = (44100, 48000)


def start_command(child, command):
    send_line(child, command + "; printf '\\nmedia-exit:%s\\n' \"$?\"")


def finish_command(child, success=True):
    child.expect(BOOT_PROMPT, timeout=30)
    output = child.before
    status = re.search(r"(?:^|\n)media-exit:(\d+)[\r\n]", output)
    if not status or (status[1] == "0") != success:
        raise AssertionError(f"unexpected guest exit status:\n{output}")
    return output


def run_checked(child, command, success=True):
    start_command(child, command)
    return finish_command(child, success)


def verify_guest(child):
    child.expect(BOOT_PROMPT, timeout=45)
    run_checked(child, "mount -t ext2 -o ro /dev/hdd /mnt")
    for rate in RATES:
        output = run_checked(
            child,
            "/mnt/mpv -v --no-config --no-input-terminal --no-video --ao=null "
            "--no-msg-color --msg-level=ao/null=trace "
            f"/mnt/stereo-markers-{rate}.mp4",
        )
        for marker in (
            "Selected decoder: aac",
            f"AO: [null] {rate}Hz stereo",
            "(End of file)",
        ):
            if marker not in output:
                raise AssertionError(f"{rate} Hz: missing {marker!r}\n{output}")
        frames = sum(map(int, re.findall(r"\[ao/null\] in=(\d+) space=", output)))
        if not 2 * rate <= frames < 2 * rate + 1024:
            raise AssertionError(
                f"{rate} Hz: expected two seconds at null AO, got {frames} frames"
            )

    output = run_checked(
        child,
        "/mnt/mpv -v --no-config --no-input-terminal --no-audio --vo=null "
        "--untimed --framedrop=no --no-msg-color --msg-level=cplayer=trace "
        "/mnt/flash-beep.mp4",
    )
    for marker in (
        "Selected decoder: h264",
        "VO: [null] 320x180 yuv420p",
        "(End of file)",
    ):
        if marker not in output:
            raise AssertionError(f"H.264: missing {marker!r}\n{output}")
    frames = output.count("[cplayer] frametime=")
    if frames != 60:
        raise AssertionError(f"H.264: expected 60 decoded frames, got {frames}")


@contextmanager
def media_disk(files=(), assets=()):
    with tempfile.TemporaryDirectory(prefix="guest-", dir=MEDIA / "build") as directory:
        work = Path(directory)
        staging = work / "root"
        staging.mkdir()
        shutil.copy2(MPV, staging)
        for rate in RATES:
            shutil.copy2(ASSETS / f"stereo-markers-{rate}.mp4", staging)
        for name in assets:
            shutil.copy2(ASSETS / name, staging)
        for name, contents in files:
            (staging / name).write_text(contents, encoding="ascii")
        image = work / "media.img"
        subprocess.run(
            ["mke2fs", "-q", "-t", "ext2", "-d", staging, image, "64M"], check=True
        )

        yield image


@contextmanager
def media_command(smp, files=(), assets=()):
    with media_disk(files, assets) as image:
        yield qemu_command(QEMU, smp) + [
            "-drive",
            f"id=media,file={image},format=raw,if=none",
            "-device",
            "ide-hd,drive=media,bus=mario-test-ahci.3",
        ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()

    if not MPV.is_file():
        parser.error("mpv is missing; run make app first")
    require_files(require_kernel=True)

    logs = Path(tempfile.mkdtemp(prefix="mario-mpv-"))
    with media_command(args.smp, assets=("flash-beep.mp4",)) as cmd:
        with (logs / "serial.log").open("w", encoding="latin-1") as serial:
            child = pexpect.spawn(cmd[0], cmd[1:], encoding="latin-1")
            child.logfile_read = serial
            try:
                make_pexpect_raw(child)
                verify_guest(child)
            except (AssertionError, pexpect.ExceptionPexpect) as error:
                print(f"FAIL: mpv decode (SMP={args.smp}): {error}; logs: {logs}")
                return 1
            finally:
                child.close(force=True)

    print(f"PASS: mpv decode (SMP={args.smp}); logs: {logs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
