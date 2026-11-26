"""Exercise the installed network utilities in an isolated Mario guest."""

import argparse
import shlex
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from boot import stage_fixture
from ethernet import SOURCES, finish, guest, run, start, wait_status
from https import certificates, forward
from lit_kernel import kernel_artifacts
from lit_shell_test import require_files

HERE = Path(__file__).resolve().parent
HTTP_PORT = 24680
HTTPS_PORT = 24681
GUEST_PYTHON = "/usr/bin/python -B /mnt/tools_guest.py"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    args = parser.parse_args()
    require_files(require_kernel=True)
    with kernel_artifacts(None), TemporaryDirectory(prefix="mario-net-tools-") as temp:
        directory = Path(temp)
        certificates(directory)
        initrd = stage_fixture(
            directory,
            {"netctl": SOURCES["netctl"]},
            files=[
                HERE / name
                for name in (
                    "tools_guest.py",
                    "python_guest.py",
                    "dns_fixture.py",
                    "http_fixture.py",
                    "tls_fixture.py",
                )
            ]
            + [directory / "ca.pem"],
        )
        http = shlex.join([sys.executable, str(HERE / "http_fixture.py")])
        backend = (
            "user,id=wire,restrict=on"
            f",guestfwd=tcp:10.0.2.100:{HTTP_PORT}-cmd:{http}"
            + forward(directory, HTTPS_PORT, "server.pem")
        )
        with guest(args.smp, directory, initrd, backend) as (child, _monitor):
            run(child, "/sbin/netctl.exe dhcp start")
            wait_status(
                child, r"source=dhcp address=10\.0\.2\.15/24.*\r*\ndhcp=bound", 12
            )
            start(
                child,
                f"{GUEST_PYTHON} {HTTP_PORT} {HTTPS_PORT}",
            )
            result = finish(child, timeout=120)
            assert "NET_TOOLS_OK" in result, result
            start(child, f"{GUEST_PYTHON} --views")
            result = finish(child, timeout=60)
            assert "NET_TOOLS_VIEWS_OK" in result, result
        with guest(args.smp, directory, initrd, None) as (child, _monitor):
            start(child, f"{GUEST_PYTHON} --no-nic")
            result = finish(child, timeout=60)
            assert "NET_TOOLS_NO_NIC_OK" in result, result
        print(f"PASS: installed network tools (SMP={args.smp})", flush=True)


if __name__ == "__main__":
    main()
