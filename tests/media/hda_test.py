"""Boot focused Intel HDA tests without a host audio device."""

import argparse
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lit_shell_test import BOOT_PROMPT, QEMU, qemu_command
from pexpect_utils import make_pexpect_raw, send_line
import pexpect


def verify_probe(child, bridge):
    child.expect(BOOT_PROMPT)
    send_line(child, "dmesg")
    child.expect(BOOT_PROMPT)
    boot = child.before
    if boot.count("; unmapped") != 2 or (bridge and "probe 1:" not in boot):
        raise AssertionError("expected two stable HDA probes on the requested buses")
    if not 0 <= boot.find("AHCI controller found") < boot.find("probe "):
        raise AssertionError("AHCI must already be mapped while probing HDA")
    send_line(child, "/usr/bin/test/ahci-completion.exe")
    child.expect(BOOT_PROMPT)
    if "preserve exact sector bytes and bounded completion" not in child.before:
        raise AssertionError("AHCI read check failed after HDA probe teardown")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, choices=(1, 2), default=1)
    parser.add_argument(
        "--bridge", action="store_true", help="probe a secondary PCI bus"
    )
    args = parser.parse_args()
    cmd = qemu_command(QEMU, args.smp) + ["-append", "hda=probe"]
    cmd += ["-device", "ich9-intel-hda,id=hda-root,msi=off"]
    secondary = "ich9-intel-hda,id=hda-secondary,msi=off"
    if args.bridge:
        cmd += ["-device", "pci-bridge,id=hda-bridge,chassis_nr=1"]
        secondary += ",bus=hda-bridge"
    cmd += ["-device", secondary]
    logs = Path(tempfile.mkdtemp(prefix="mario-hda-"))
    with (logs / "serial.log").open("w", encoding="latin-1") as serial:
        child = pexpect.spawn(cmd[0], cmd[1:], encoding="latin-1", timeout=45)
        child.logfile_read = serial
        try:
            make_pexpect_raw(child)
            verify_probe(child, args.bridge)
        except (AssertionError, pexpect.ExceptionPexpect) as error:
            print(f"FAIL: HDA MMIO (SMP={args.smp}): {error}; logs: {logs}")
            return 1
        finally:
            child.close(force=True)
    print(f"PASS: HDA MMIO (SMP={args.smp}); logs: {logs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
