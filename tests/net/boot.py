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


def stage_fixture(directory):
    contents = directory / "contents"
    contents.mkdir()
    subprocess.run(
        [
            ROOT / "util/mario-musl-cc",
            "-Wall",
            "-Werror",
            "-Os",
            "-s",
            "-o",
            contents / "net-loopback",
            Path(__file__).with_name("loopback.c"),
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


def check_boot(smp):
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
            send_line(
                child, "/mnt/net-loopback; printf '\\nNET_LOOPBACK_STATUS=%s\\n' $?"
            )
            child.expect(r"(?m)^NET_LOOPBACK_STATUS=(\d+)\r*$", timeout=25)
            if child.match.group(1) != "0" or "NET_LOOPBACK_OK" not in child.before:
                raise RuntimeError(f"loopback fixture failed\n{child.before[-3000:]}")
        finally:
            child.close(force=True)
    print(f"PASS: TCP and UDP loopback (SMP={smp})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    args = parser.parse_args()
    require_files(require_kernel=True)
    check_boot(args.smp)


if __name__ == "__main__":
    main()
