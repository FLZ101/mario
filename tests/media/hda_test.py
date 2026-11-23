"""Boot focused Intel HDA tests without a host audio device."""

import argparse
from pathlib import Path
import socket
import struct
import sys
import tempfile
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lit_shell_test import BOOT_PROMPT, QEMU, qemu_command
from pexpect_utils import make_pexpect_raw, send_line
import pexpect


def boot_log(child):
    child.expect(BOOT_PROMPT)
    send_line(child, "dmesg")
    child.expect(BOOT_PROMPT)
    return child.before


def verify_disk(child):
    send_line(child, "/usr/bin/test/ahci-completion.exe")
    child.expect(BOOT_PROMPT)
    if "preserve exact sector bytes and bounded completion" not in child.before:
        raise AssertionError("AHCI read check failed after HDA probe teardown")


def verify_wav(path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (48000, 2, 2):
            raise AssertionError("expected 48 kHz stereo S16 capture")
        frames = list(struct.iter_unpack("<hh", wav.readframes(wav.getnframes())))
    markers = [index for index, (left, right) in enumerate(frames) if left or right]
    if len(markers) != 960 or markers[-1] - markers[0] != 959:
        raise AssertionError(
            f"expected exactly 960 consecutive marker frames, got {len(markers)}"
        )
    for index, (left, right) in enumerate(frames[markers[0] : markers[-1] + 1]):
        if index < 480:
            sign = 1 if index % 96 < 48 else -1
            if right != 0 or not 1000 <= left * sign <= 12000:
                raise AssertionError(f"wrong left-channel marker at frame {index}")
        else:
            sign = 1 if (index - 480) % 64 < 32 else -1
            if left != 0 or not 1000 <= right * sign <= 8000:
                raise AssertionError(f"wrong right-channel marker at frame {index}")


def verify_guest(child, args):
    boot = boot_log(child)
    if args.mode == "probe":
        if boot.count("; unmapped") != 2 or (args.bridge and "probe 1:" not in boot):
            raise AssertionError("expected two stable HDA probes on the requested buses")
        if not 0 <= boot.find("AHCI controller found") < boot.find("probe "):
            raise AssertionError("AHCI must already be mapped while probing HDA")
    elif args.mode == "pcm":
        if not 0 <= boot.find("no codecs detected") < boot.find("PCM self-test passed"):
            raise AssertionError(
                "expected a codec-less controller followed by successful PCM"
            )
    else:
        if "no playback device" not in boot or "PCM self-test passed" in boot:
            raise AssertionError("expected a normal boot without playback")
        if args.mode == "no-codec" and "no codecs detected" not in boot:
            raise AssertionError("expected the controller to reject an empty codec bus")
    verify_disk(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1)
    parser.add_argument(
        "--bridge", action="store_true", help="probe a secondary PCI bus"
    )
    parser.add_argument(
        "--mode", choices=("probe", "pcm", "no-codec", "no-controller"), default="probe"
    )
    args = parser.parse_args()
    if args.bridge and args.mode != "probe":
        parser.error("--bridge is only available with --mode probe")
    logs = Path(tempfile.mkdtemp(prefix="mario-hda-"))
    wav = logs / "capture.wav"
    monitor = logs / "monitor.sock"
    cmd = qemu_command(QEMU, args.smp)
    cmd += ["-monitor", f"unix:{monitor},server=on,wait=off"]
    cmd += ["-append", "hda=probe" if args.mode == "probe" else "hda=selftest"]
    if args.mode != "no-controller":
        cmd += ["-device", "ich9-intel-hda,id=hda-root,msi=off"]
    if args.mode in ("probe", "pcm"):
        secondary = "ich9-intel-hda,id=hda-secondary,msi=off"
        if args.bridge:
            cmd += ["-device", "pci-bridge,id=hda-bridge,chassis_nr=1"]
            secondary += ",bus=hda-bridge"
        cmd += ["-device", secondary]
    if args.mode == "pcm":
        cmd += [
            "-device",
            "hda-output,bus=hda-secondary.0,audiodev=mario-audio",
            "-audiodev",
            f"wav,id=mario-audio,path={wav},timer-period=1000,"
            "out.frequency=48000,out.channels=2,out.format=s16",
        ]
    with (logs / "serial.log").open("w", encoding="latin-1") as serial:
        child = pexpect.spawn(cmd[0], cmd[1:], encoding="latin-1", timeout=45)
        child.logfile_read = serial
        try:
            make_pexpect_raw(child)
            verify_guest(child, args)
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(str(monitor))
                connection.sendall(b"quit\n")
                child.expect(pexpect.EOF, timeout=10)
            child.close()
            if child.exitstatus != 0:
                raise AssertionError("QEMU did not exit cleanly")
            if args.mode == "pcm":
                verify_wav(wav)
        except (
            AssertionError,
            pexpect.ExceptionPexpect,
            OSError,
            wave.Error,
            EOFError,
        ) as error:
            print(f"FAIL: HDA {args.mode} (SMP={args.smp}): {error}; logs: {logs}")
            return 1
        finally:
            child.close(force=True)
    print(f"PASS: HDA {args.mode} (SMP={args.smp}); logs: {logs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
