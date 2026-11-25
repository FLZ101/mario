"""Boot focused Intel HDA tests without a host audio device."""

import argparse
from pathlib import Path
import re
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
    output = run_checked(child, "/usr/bin/test/ahci-completion.exe")
    if "preserve exact sector bytes and bounded completion" not in output:
        raise AssertionError("AHCI read check failed after HDA probe teardown")


def wav_frames(path):
    with wave.open(str(path), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (
            48000,
            2,
            2,
        ):
            raise AssertionError("expected 48 kHz stereo S16 capture")
        return list(struct.iter_unpack("<hh", wav.readframes(wav.getnframes())))


def verify_wav(path):
    frames = wav_frames(path)
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


def verify_device_capture(path, pause):
    fixture = Path(__file__).resolve().parent / "assets" / "stereo-markers.wav"
    expected_markers = [
        (index, frame) for index, frame in enumerate(wav_frames(fixture)) if any(frame)
    ]
    actual_markers = [
        (index, frame) for index, frame in enumerate(wav_frames(path)) if any(frame)
    ]
    expected = [frame for _, frame in expected_markers]
    expected += [(2345, 0)] * 123 + [(0, -3456)] * 123
    actual = [frame for _, frame in actual_markers]
    for index, (got, want) in enumerate(zip(actual, expected)):
        if got != want:
            raise AssertionError(f"nonzero frame {index}: expected {want}, got {got}")
    if len(actual) != len(expected):
        raise AssertionError(
            f"expected {len(expected)} nonzero frames, got {len(actual)}"
        )
    for index in range(1, len(expected_markers)):
        before, after = expected_markers[index - 1][0], expected_markers[index][0]
        extra = (
            actual_markers[index][0] - actual_markers[index - 1][0] - (after - before)
        )
        if extra != 0 and not (before < pause <= after and 0 < extra < 48000 // 5):
            raise AssertionError(
                f"unexpected silence change of {extra} frames at {after}"
            )


def verify_late_capture(path):
    actual = [frame for frame in wav_frames(path) if any(frame)]
    expected = [(4100, 0)] * 123 + [(2345, 0)] * 123
    if actual != expected:
        raise AssertionError("missed poll replayed a marker or broke recovery playback")


def run_checked(child, command):
    send_line(child, command)
    if child.expect([BOOT_PROMPT, r"Assertion failed:[^\r\n]+[\r\n]"]):
        assertion = child.after.rstrip()
        try:
            child.expect(BOOT_PROMPT, timeout=2)
            send_line(child, "dmesg")
            child.expect(BOOT_PROMPT, timeout=2)
        except pexpect.ExceptionPexpect:
            pass
        raise AssertionError(assertion)
    return child.before


def verify_guest(child, args):
    boot = boot_log(child)
    if args.mode == "probe":
        if boot.count("; unmapped") != 2 or (args.bridge and "probe 1:" not in boot):
            raise AssertionError(
                "expected two stable HDA probes on the requested buses"
            )
        if not 0 <= boot.find("AHCI controller found") < boot.find("probe "):
            raise AssertionError("AHCI must already be mapped while probing HDA")
    elif args.mode == "pcm":
        if not 0 <= boot.find("no codecs detected") < boot.find("PCM self-test passed"):
            raise AssertionError(
                "expected a codec-less controller followed by successful PCM"
            )
    elif args.mode == "device":
        if "PCM device ready" not in boot:
            raise AssertionError("expected a usable PCM device")
        output = run_checked(child, "/usr/bin/test/pcm.exe stress")
        for marker in (
            "wrap, pause, drain, reset, underrun and reopen passed",
            "concurrent AHCI reads preserved sector bytes",
        ):
            if marker not in output:
                raise AssertionError(f"guest did not report: {marker}")
        pause = re.search(r"pcm: paused after (\d+) fixture frames", output)
        if not pause or not 12000 < int(pause[1]) < 24000:
            raise AssertionError("expected pause to occur inside the left marker")
        return int(pause[1])
    elif args.mode == "late":
        output = run_checked(child, "/usr/bin/test/pcm.exe late")
        if "missed poll invalidates position, and playback restarts" not in output:
            raise AssertionError("guest did not report missed-poll recovery")
    else:
        if "no playback device" not in boot or "PCM self-test passed" in boot:
            raise AssertionError("expected a normal boot without playback")
        if args.mode == "no-codec" and "no codecs detected" not in boot:
            raise AssertionError("expected the controller to reject an empty codec bus")
        output = run_checked(child, "/usr/bin/test/pcm.exe absent")
        if "unavailable hardware returns ENODEV" not in output:
            raise AssertionError("expected ENODEV when opening PCM without hardware")
    verify_disk(child)


def quit_qemu(child, monitor):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(monitor))
        connection.sendall(b"quit\n")
        child.expect(pexpect.EOF, timeout=10)
    child.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1)
    parser.add_argument(
        "--bridge", action="store_true", help="probe a secondary PCI bus"
    )
    parser.add_argument(
        "--mode",
        choices=("probe", "pcm", "device", "late", "no-codec", "no-controller"),
        default="probe",
    )
    parser.add_argument(
        "--listen", choices=("pa", "pipewire"), help="play using GTK and host audio"
    )
    args = parser.parse_args()
    if args.bridge and args.mode != "probe":
        parser.error("--bridge is only available with --mode probe")
    if args.listen and args.mode != "device":
        parser.error("--listen is only available with --mode device")
    logs = Path(tempfile.mkdtemp(prefix="mario-hda-"))
    wav = logs / "capture.wav"
    monitor = logs / "monitor.sock"
    cmd = qemu_command(QEMU, args.smp)
    cmd += ["-monitor", f"unix:{monitor},server=on,wait=off"]
    boot_option = {"probe": "probe", "pcm": "selftest", "late": "test-late"}.get(
        args.mode
    )
    if boot_option:
        cmd += ["-append", f"hda={boot_option}"]
    if args.mode != "no-controller":
        cmd += ["-device", "ich9-intel-hda,id=hda-root,msi=off"]
    if args.mode in ("probe", "pcm", "device", "late"):
        secondary = "ich9-intel-hda,id=hda-secondary,msi=off"
        if args.bridge:
            cmd += ["-device", "pci-bridge,id=hda-bridge,chassis_nr=1"]
            secondary += ",bus=hda-bridge"
        cmd += ["-device", secondary]
    if args.mode in ("pcm", "device", "late"):
        backend = args.listen or f"wav,path={wav}"
        codec = "hda-output,bus=hda-secondary.0,audiodev=mario-audio"
        if args.listen:
            codec += ",mixer=off"
        cmd += [
            "-device",
            codec,
            "-audiodev",
            f"{backend},id=mario-audio,timer-period=1000,"
            "out.frequency=48000,out.channels=2,out.format=s16",
        ]
    if args.listen:
        cmd[cmd.index("-display") + 1] = "gtk"
    with (logs / "serial.log").open("w", encoding="latin-1") as serial:
        child = pexpect.spawn(cmd[0], cmd[1:], encoding="latin-1", timeout=45)
        child.logfile_read = serial
        try:
            make_pexpect_raw(child)
            pause = verify_guest(child, args)
            quit_qemu(child, monitor)
            if child.exitstatus != 0:
                raise AssertionError("QEMU did not exit cleanly")
            if args.mode == "pcm":
                verify_wav(wav)
            elif args.mode == "device" and not args.listen:
                verify_device_capture(wav, pause)
            elif args.mode == "late":
                verify_late_capture(wav)
        except (
            AssertionError,
            pexpect.ExceptionPexpect,
            OSError,
            wave.Error,
            EOFError,
        ) as error:
            if child.isalive() and monitor.exists():
                try:
                    quit_qemu(child, monitor)
                except (OSError, pexpect.ExceptionPexpect):
                    pass
            print(f"FAIL: HDA {args.mode} (SMP={args.smp}): {error}; logs: {logs}")
            return 1
        finally:
            child.close(force=True)
    print(f"PASS: HDA {args.mode} (SMP={args.smp}); logs: {logs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
