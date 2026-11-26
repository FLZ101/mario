"""Verify IPv4 loopback without a NIC."""

import argparse
import re
import shutil
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory

import pexpect

from lit_kernel import ROOT, kernel_artifacts
from lit_shell_test import BOOT_PROMPT, QEMU, qemu_command, require_files
from pexpect_utils import make_pexpect_raw, send_line

FIXTURES = {
    "loopback": "NET_LOOPBACK_OK",
    "socket-semantics": "NET_SEMANTICS_OK",
    "resolver": "NET_RESOLVER_OK",
}


def stage_fixture(directory):
    contents = directory / "contents"
    contents.mkdir()
    for fixture in FIXTURES:
        subprocess.run(
            [
                ROOT / "util/mario-musl-cc",
                "-Wall",
                "-Werror",
                "-Os",
                "-s",
                "-o",
                contents / fixture,
                Path(__file__).with_name(f"{fixture}.c"),
            ],
            cwd=ROOT,
            check=True,
        )
    fixture = directory / "fixture.img"
    subprocess.run(
        [ROOT / "rd/rd-make.exe", f"--dir={contents}", f"--rd={fixture}"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    initrd = directory / "initrd.img"
    with initrd.open("wb") as output:
        for image in (ROOT / "rd/rdx", fixture):
            with image.open("rb") as source:
                shutil.copyfileobj(source, output)
    return initrd


def check_boot(smp, time_wait):
    command = qemu_command(QEMU, smp)
    command += ["-nic", "none"]
    marker = "net: lwIP worker ready"
    with kernel_artifacts(None), TemporaryDirectory() as directory:
        command[command.index("-initrd") + 1] = str(stage_fixture(Path(directory)))
        child = pexpect.spawn(command[0], command[1:], encoding="latin-1", timeout=30)
        try:
            make_pexpect_raw(child)
            child.expect(BOOT_PROMPT)
            send_line(child, "dmesg; printf '\\nNET_BOOT_DONE\\n'")
            child.expect(r"(?m)^NET_BOOT_DONE\r*$", timeout=5)
            output = re.sub(r"\x1b\[[0-9;]*m", "", child.before)
            if marker not in output:
                raise RuntimeError(f"missing boot marker {marker!r}\n{output[-3000:]}")
            send_line(
                child, "mknod /root/net-rd b 1 3 && mount -t mariofs /root/net-rd /mnt"
            )
            child.expect(BOOT_PROMPT)
            cases = (
                {"socket-semantics time-wait": "NET_TIME_WAIT_OK"}
                if time_wait
                else FIXTURES
            )
            for fixture, success in cases.items():
                send_line(child, f"/mnt/{fixture}; printf '\\nNET_STATUS=%s\\n' $?")
                child.expect(
                    r"(?m)^NET_STATUS=(\d+)\r*\n", timeout=160 if time_wait else 25
                )
                if child.match.group(1) != "0" or success not in child.before:
                    raise RuntimeError(f"{fixture} failed\n{child.before[-3000:]}")
                child.expect(BOOT_PROMPT)
                print(f"PASS: {fixture} (SMP={smp})", flush=True)
        finally:
            child.close(force=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    parser.add_argument("--time-wait", action="store_true")
    args = parser.parse_args()
    require_files(require_kernel=True)
    check_boot(args.smp, args.time_wait)


if __name__ == "__main__":
    main()
