"""Verify that the lwIP worker starts without a NIC."""

import argparse
import re

import pexpect

from lit_kernel import kernel_artifacts
from lit_shell_test import BOOT_PROMPT, QEMU, qemu_command, require_files
from pexpect_utils import make_pexpect_raw, send_line


def check_boot(smp):
    command = qemu_command(QEMU, smp)
    command += ["-nic", "none"]
    marker = "net: lwIP worker ready"
    with kernel_artifacts(None):
        child = pexpect.spawn(command[0], command[1:], encoding="latin-1", timeout=30)
        try:
            make_pexpect_raw(child)
            child.expect(BOOT_PROMPT)
            send_line(child, "dmesg; printf '\\nNET_BOOT_DONE\\n'")
            child.expect(r"(?m)^NET_BOOT_DONE\r*$", timeout=5)
            output = re.sub(r"\x1b\[[0-9;]*m", "", child.before)
            if marker not in output:
                raise RuntimeError(f"missing boot marker {marker!r}\n{output[-3000:]}")
        finally:
            child.close(force=True)
    print(f"PASS: network boot (SMP={smp})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    args = parser.parse_args()
    require_files(require_kernel=True)
    check_boot(args.smp)


if __name__ == "__main__":
    main()
