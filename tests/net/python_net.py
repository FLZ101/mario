"""Run DHCP and Python networking integration tests in disposable QEMU guests."""

import argparse
import shlex
import sys
from http.client import HTTPConnection
from pathlib import Path
from tempfile import TemporaryDirectory

from boot import stage_fixture
from dhcp import check_dhcp
from ethernet import SOURCES, finish, guest, listener, run, start, wait_status
from http_fixture import BODY
from lit_kernel import kernel_artifacts
from lit_shell_test import require_files

PORT = 24680
PYTHON = "/usr/bin/python -B /mnt/python_guest.py"
HERE = Path(__file__).resolve().parent


def check_python(smp, directory, initrd):
    with listener() as reservation:
        inbound = reservation.getsockname()[1]
    outbound = shlex.join([sys.executable, str(HERE / "http_fixture.py")])
    backend = (
        f"user,id=wire,restrict=on,hostfwd=tcp:127.0.0.1:{inbound}-10.0.2.15:{PORT},"
        f"guestfwd=tcp:10.0.2.100:{PORT}-cmd:{outbound}"
    )
    with guest(smp, directory, initrd, backend) as (child, _monitor):
        run(child, "/sbin/netctl.exe dhcp start")
        wait_status(child, r"source=dhcp address=10\.0\.2\.15/24.*\r*\ndhcp=bound", 12)
        start(child, f"{PYTHON} {PORT}")
        ready = child.expect(
            [r"(?m)^PY_NET_READY\r*$", r"(?m)^ETH_STATUS=\d+\r*$"], timeout=90
        )
        assert ready == 0, child.before
        connection = HTTPConnection("127.0.0.1", inbound, timeout=5)
        try:
            connection.request("GET", "/payload")
            response = connection.getresponse()
            assert (
                response.status == 200
                and response.getheader("X-Mario-Fixture") == "net"
            )
            assert response.read() == BODY
        finally:
            connection.close()
        assert "PY_NET_OK" in finish(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    parser.add_argument("--case", choices=("dhcp", "python"))
    args = parser.parse_args()
    require_files(require_kernel=True)
    with kernel_artifacts(None), TemporaryDirectory(prefix="mario-pynet-") as temp:
        directory = Path(temp)
        initrd = stage_fixture(
            directory,
            {
                "netctl": SOURCES["netctl"],
                "loopback": HERE / "loopback.c",
                "resolver": HERE / "resolver.c",
            },
            files=[
                HERE / name
                for name in ("python_guest.py", "dns_fixture.py", "http_fixture.py")
            ],
        )
        for name, test in (("dhcp", check_dhcp), ("python", check_python)):
            if args.case and args.case != name:
                continue
            case_dir = directory / name
            case_dir.mkdir()
            test(args.smp, case_dir, initrd)
            print(f"PASS: {name} networking (SMP={args.smp})", flush=True)


if __name__ == "__main__":
    main()
